// esp-relay — ретранслятор MJPEG-потока ESP32-CAM на произвольное число клиентов.
//
// Стандартная прошивка CameraWebServer держит /stream в одном TCP-соединении:
// второй клиент получает «занято». Этот сервис открывает к камере одно постоянное
// соединение, парсит multipart/x-mixed-replace и раздаёт каждый JPEG-кадр всем
// подписанным клиентам по схеме fan-out через каналы. Медленные клиенты
// автоматически теряют кадры, не блокируя источник.
//
// Запуск:
//
//	esp-relay -src=http://<cam-ip>:81/stream -listen=:8032
//
// Эндпоинты:
//
//	GET /         — HTML-страница с <img src="/stream">
//	GET /stream   — MJPEG (multipart/x-mixed-replace)
//	GET /snapshot — последний полученный кадр в виде JPEG
package main

import (
	"bytes"
	"flag"
	"fmt"
	"io"
	"log"
	"mime"
	"mime/multipart"
	"net/http"
	"strings"
	"sync"
	"time"
)

// outBoundary — разделитель, который сервис указывает в Content-Type
// собственного multipart-ответа клиентам. С boundary исходного потока
// от ESP32-CAM не связан — там парсится свой, взятый из заголовка ответа камеры.
const outBoundary = "frame"

// stallTimeout — сторожевой интервал чтения исходного потока. Если камера
// умирает без TCP FIN (brownout, обрыв питания), сокет остаётся полуоткрытым
// и чтение блокируется навсегда; по истечении этого срока без единого кадра
// соединение принудительно закрывается и puller переподключается.
const stallTimeout = 10 * time.Second

// Hub хранит подписчиков и последний полученный кадр.
//
// Все операции потокобезопасны. На каждого клиента заводится буферизованный
// канал ёмкостью 1: при попытке отправить новый кадр клиенту, который ещё
// не забрал предыдущий, кадр просто отбрасывается. Это сознательный компромисс:
// один тормозящий зритель не должен задерживать поток для остальных и для
// читателя из камеры.
type Hub struct {
	mu      sync.Mutex
	clients map[chan []byte]struct{}
	latest  []byte // последний разосланный кадр; отдаётся новым подписчикам сразу при подключении
}

func newHub() *Hub {
	return &Hub{clients: make(map[chan []byte]struct{})}
}

// subscribe регистрирует нового клиента и возвращает канал, в который будут
// поступать кадры. Если в hub уже есть кэшированный latest-кадр, он сразу
// кладётся в канал — клиент видит картинку без ожидания следующего кадра.
func (h *Hub) subscribe() chan []byte {
	ch := make(chan []byte, 1)
	h.mu.Lock()
	h.clients[ch] = struct{}{}
	if h.latest != nil {
		select {
		case ch <- h.latest:
		default:
		}
	}
	h.mu.Unlock()
	return ch
}

// unsubscribe удаляет клиента и закрывает его канал. Идемпотентна.
func (h *Hub) unsubscribe(ch chan []byte) {
	h.mu.Lock()
	if _, ok := h.clients[ch]; ok {
		delete(h.clients, ch)
		close(ch)
	}
	h.mu.Unlock()
}

// broadcast рассылает кадр всем подписчикам и обновляет latest.
// Отправка в каждый канал неблокирующая (select { default: }), поэтому
// один зависший клиент не тормозит ни источник, ни остальных зрителей.
func (h *Hub) broadcast(frame []byte) {
	h.mu.Lock()
	h.latest = frame
	for ch := range h.clients {
		select {
		case ch <- frame:
		default:
		}
	}
	h.mu.Unlock()
}

// snapshot возвращает последний полученный кадр (или nil, если ещё ни одного не было).
func (h *Hub) snapshot() []byte {
	h.mu.Lock()
	defer h.mu.Unlock()
	return h.latest
}

// count — текущее число подписчиков; используется только для логов.
func (h *Hub) count() int {
	h.mu.Lock()
	defer h.mu.Unlock()
	return len(h.clients)
}

// puller бесконечно поддерживает соединение с камерой. При любой ошибке
// (обрыв сети, перезагрузка ESP, неправильный Content-Type) ждёт 2 секунды
// и переподключается. Запускается в отдельной goroutine из main.
func puller(srcURL string, hub *Hub) {
	client := &http.Client{Timeout: 0} // 0 — стрим живёт бесконечно
	for {
		if err := pullOnce(client, srcURL, hub); err != nil {
			log.Printf("source error: %v — reconnect in 2s", err)
		}
		time.Sleep(2 * time.Second)
	}
}

// pullOnce открывает одно соединение к камере и читает кадры из multipart-потока,
// пока соединение живо. Возвращает ошибку при любом обрыве — реконнект делает puller.
func pullOnce(client *http.Client, srcURL string, hub *Hub) error {
	resp, err := client.Get(srcURL)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("source status %d", resp.StatusCode)
	}

	mediaType, params, err := mime.ParseMediaType(resp.Header.Get("Content-Type"))
	if err != nil {
		return fmt.Errorf("parse content-type: %w", err)
	}
	if !strings.HasPrefix(mediaType, "multipart/") {
		return fmt.Errorf("unexpected content-type: %s", mediaType)
	}

	log.Printf("connected to source: %s (boundary=%s)", srcURL, params["boundary"])

	// Сторожевой таймер: закрывает тело ответа, если кадры перестали приходить.
	// Закрытие выводит заблокированное чтение из NextPart/ReadAll с ошибкой,
	// дальше обычный реконнект в puller.
	watchdog := time.AfterFunc(stallTimeout, func() {
		log.Printf("source stalled: no frame for %v — closing connection", stallTimeout)
		resp.Body.Close()
	})
	defer watchdog.Stop()

	mr := multipart.NewReader(resp.Body, params["boundary"])
	for {
		part, err := mr.NextPart()
		if err != nil {
			return fmt.Errorf("next part: %w", err)
		}
		buf, err := io.ReadAll(part)
		part.Close()
		if err != nil {
			return fmt.Errorf("read part: %w", err)
		}
		watchdog.Reset(stallTimeout)
		if len(buf) == 0 {
			continue
		}
		hub.broadcast(buf)
	}
}

// streamHandler отдаёт клиенту бесконечный multipart/x-mixed-replace.
// Для каждого подключения создаётся свой канал в hub; завершение
// соединения (контекст запроса отменён) гарантированно вызывает unsubscribe.
func streamHandler(hub *Hub) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		flusher, ok := w.(http.Flusher)
		if !ok {
			http.Error(w, "streaming unsupported", http.StatusInternalServerError)
			return
		}

		w.Header().Set("Content-Type", "multipart/x-mixed-replace; boundary="+outBoundary)
		w.Header().Set("Cache-Control", "no-cache, private")
		w.Header().Set("Pragma", "no-cache")
		w.Header().Set("Access-Control-Allow-Origin", "*")

		ch := hub.subscribe()
		defer hub.unsubscribe(ch)

		log.Printf("client connected: %s (total=%d)", r.RemoteAddr, hub.count())
		defer func() { log.Printf("client disconnected: %s (total=%d)", r.RemoteAddr, hub.count()-1) }()

		var buf bytes.Buffer
		ctx := r.Context()
		for {
			select {
			case <-ctx.Done():
				return
			case frame, ok := <-ch:
				if !ok {
					return
				}
				buf.Reset()
				fmt.Fprintf(&buf, "--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n", outBoundary, len(frame))
				buf.Write(frame)
				buf.WriteString("\r\n")
				if _, err := w.Write(buf.Bytes()); err != nil {
					return
				}
				flusher.Flush()
			}
		}
	}
}

// snapshotHandler отдаёт текущий кадр одиночным JPEG. Удобно для cron-скриптов,
// дашбордов или превью без накладных расходов на multipart.
func snapshotHandler(hub *Hub) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		frame := hub.snapshot()
		if frame == nil {
			http.Error(w, "no frame yet", http.StatusServiceUnavailable)
			return
		}
		w.Header().Set("Content-Type", "image/jpeg")
		w.Header().Set("Cache-Control", "no-cache")
		w.Header().Set("Access-Control-Allow-Origin", "*")
		w.Write(frame)
	}
}

// indexHandler — минимальная HTML-страница, которая просто вставляет /stream
// в <img>. Достаточна для просмотра в любом браузере без плееров.
func indexHandler(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" {
		http.NotFound(w, r)
		return
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	fmt.Fprint(w, `<!doctype html>
<html><head><title>ESP32-CAM relay</title></head>
<body style="margin:0;background:#000;display:flex;align-items:center;justify-content:center;min-height:100vh">
<img src="/stream" style="max-width:100%;max-height:100vh;object-fit:contain"/>
</body></html>`)
}

func main() {
	src := flag.String("src", "http://192.168.1.50:81/stream", "ESP32-CAM MJPEG stream URL")
	listen := flag.String("listen", ":8080", "listen address")
	flag.Parse()

	hub := newHub()
	go puller(*src, hub)

	mux := http.NewServeMux()
	mux.HandleFunc("/", indexHandler)
	mux.HandleFunc("/stream", streamHandler(hub))
	mux.HandleFunc("/snapshot", snapshotHandler(hub))

	log.Printf("source : %s", *src)
	log.Printf("listen : %s   (open http://<this-pc-ip>%s/)", *listen, *listen)
	log.Fatal(http.ListenAndServe(*listen, mux))
}
