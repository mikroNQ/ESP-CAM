// esp-dashboard — приёмник событий ML-детектора ESP32-CAM и веб-морда с
// таймингами включения/выключения светодиодной линии сканера.
//
// Поток данных:
//
//	[ ESP32-CAM ] --NDJSON по TCP--> [ esp-dashboard :9000 ]
//	                                       |  (агрегирует состояние + статистику)
//	                                       v
//	                              [ браузер ] <--SSE-- :8080/events
//
// Прошивка шлёт по TCP построчный JSON: hello при подключении и transition на
// каждый переход состояния (off / red_on / white_on) с длительностью только что
// завершившегося состояния (dur_ms) — это и есть искомая «разница во времени»
// между включением и выключением подсветки. Сервис раздаёт обновления в браузер
// через Server-Sent Events: односторонний push без внешних зависимостей и с
// минимальной задержкой (ограничена дебаунсом прошивки, не транспортом).
//
// Запуск:
//
//	esp-dashboard -esp=:9000 -http=:8080
//
// Затем направь прошивку на этот ПК:  curl "http://<cam-ip>/detcfg?host=<pc-ip>&port=9000"
package main

import (
	"bufio"
	"bytes"
	_ "embed"
	"encoding/json"
	"flag"
	"log"
	"net"
	"net/http"
	"sync"
	"time"
)

//go:embed index.html
var indexHTML []byte

// rawEvent — построчный JSON от прошивки (NDJSON). Поля объединяют все типы
// сообщений: hello, dropped и transition.
type rawEvent struct {
	Type  string  `json:"type"`  // "hello" | "dropped" | "" для transition
	Event string  `json:"event"` // "transition"
	TsMs  uint64  `json:"ts_ms"`
	From  string  `json:"from"`
	To    string  `json:"to"`
	DurMs uint64  `json:"dur_ms"`
	Conf  float64 `json:"conf"`
	Dev   string  `json:"dev"`
	IP    string  `json:"ip"`
	Count int     `json:"count"`
}

// transition — нормализованный переход для истории и веб-морды.
type transition struct {
	Seq      uint64  `json:"seq"`
	AtUnixMs int64   `json:"at_ms"` // серверное время прихода (для часов браузера)
	From     string  `json:"from"`
	To       string  `json:"to"`
	DurMs    uint64  `json:"dur_ms"` // длительность завершившегося состояния from
	Conf     float64 `json:"conf"`
}

// stateStat — агрегаты по одному состоянию (сколько раз и как долго оно длилось).
type stateStat struct {
	Count   uint64 `json:"count"`
	TotalMs uint64 `json:"total_ms"`
	LastMs  uint64 `json:"last_ms"`
	MaxMs   uint64 `json:"max_ms"`
}

// snapshot — полное состояние, которое отдаётся браузеру (при подключении и на
// каждое событие). Браузер рендерит UI целиком из него, а живой таймер текущего
// состояния тикает локально между снапшотами.
type snapshot struct {
	Connected bool                  `json:"connected"`
	Dev       string                `json:"dev"`
	IP        string                `json:"ip"`
	Current   string                `json:"current"`
	CurConf   float64               `json:"cur_conf"`
	SinceMs   int64                 `json:"since_ms"` // когда началось текущее состояние (серверное unix ms)
	NowMs     int64                 `json:"now_ms"`   // серверное «сейчас» — для синхронизации часов браузера
	Dropped   uint64                `json:"dropped"`
	Stats     map[string]*stateStat `json:"stats"`
	History   []transition          `json:"history"` // новейшие в конце
}

const historyCap = 100

// hub — fan-out SSE-клиентам. На каждого клиента буферизованный канал; если
// клиент не успевает забирать, кадры для него отбрасываются, источник не
// блокируется (тот же приём, что в relay/).
type hub struct {
	mu      sync.Mutex
	clients map[chan []byte]struct{}
}

func newHub() *hub { return &hub{clients: make(map[chan []byte]struct{})} }

func (h *hub) subscribe() chan []byte {
	ch := make(chan []byte, 8)
	h.mu.Lock()
	h.clients[ch] = struct{}{}
	h.mu.Unlock()
	return ch
}

func (h *hub) unsubscribe(ch chan []byte) {
	h.mu.Lock()
	if _, ok := h.clients[ch]; ok {
		delete(h.clients, ch)
		close(ch)
	}
	h.mu.Unlock()
}

func (h *hub) broadcast(b []byte) {
	h.mu.Lock()
	defer h.mu.Unlock()
	for ch := range h.clients {
		select {
		case ch <- b:
		default: // медленный клиент — пропускаем кадр
		}
	}
}

// server — состояние детектора и история, потокобезопасно.
type server struct {
	mu      sync.Mutex
	hub     *hub
	conns   int // число активных TCP-подключений ESP (а не флаг: при reconnect
	            // старый обработчик не должен «погасить» связь нового)
	dev, ip string
	current   string
	curConf   float64
	since     time.Time
	seq       uint64
	dropped   uint64
	stats     map[string]*stateStat
	history   []transition
}

func newServer() *server {
	return &server{
		hub:     newHub(),
		current: "off",
		since:   time.Now(),
		stats:   make(map[string]*stateStat),
	}
}

// serveTCP принимает подключения прошивки. Обычно клиент один (сама ESP), но на
// случай переподключений каждый обрабатывается отдельной горутиной.
func (s *server) serveTCP(ln net.Listener) {
	for {
		conn, err := ln.Accept()
		if err != nil {
			log.Printf("tcp accept: %v", err)
			return
		}
		go s.handleESP(conn)
	}
}

func (s *server) handleESP(conn net.Conn) {
	addr := conn.RemoteAddr().String()
	log.Printf("ESP connected: %s", addr)
	s.connOpened()
	defer func() {
		log.Printf("ESP disconnected: %s", addr)
		s.connClosed()
		conn.Close()
	}()

	sc := bufio.NewScanner(conn)
	sc.Buffer(make([]byte, 0, 4096), 1<<16)
	// The firmware sends a keepalive "\n" every ~1s. If we see nothing for 5s the
	// link is dead (e.g. ESP lost power without a TCP FIN). A read deadline turns
	// that silence into a clean disconnect — otherwise a half-open socket blocks
	// here forever, the connection count never drops and the UI shows a stale
	// "ESP подключена". The deadline is refreshed before every read.
	for {
		conn.SetReadDeadline(time.Now().Add(5 * time.Second))
		if !sc.Scan() {
			break
		}
		s.handleLine(sc.Bytes())
	}
}

func (s *server) handleLine(b []byte) {
	var e rawEvent
	if err := json.Unmarshal(b, &e); err != nil {
		return // мусорная строка — игнорируем
	}
	switch {
	case e.Type == "hello":
		s.mu.Lock()
		s.dev, s.ip = e.Dev, e.IP
		s.mu.Unlock()
		s.pushSnapshot()
	case e.Type == "dropped":
		s.mu.Lock()
		s.dropped += uint64(e.Count)
		s.mu.Unlock()
		s.pushSnapshot()
	case e.Event == "transition":
		s.applyTransition(e)
	}
}

func (s *server) applyTransition(e rawEvent) {
	now := time.Now()
	s.mu.Lock()
	s.seq++
	tx := transition{
		Seq: s.seq, AtUnixMs: now.UnixMilli(),
		From: e.From, To: e.To, DurMs: e.DurMs, Conf: e.Conf,
	}
	st := s.stats[e.From]
	if st == nil {
		st = &stateStat{}
		s.stats[e.From] = st
	}
	st.Count++
	st.TotalMs += e.DurMs
	st.LastMs = e.DurMs
	if e.DurMs > st.MaxMs {
		st.MaxMs = e.DurMs
	}
	s.current = e.To
	s.curConf = e.Conf
	s.since = now
	s.history = append(s.history, tx)
	if len(s.history) > historyCap {
		s.history = s.history[len(s.history)-historyCap:]
	}
	s.mu.Unlock()
	s.pushSnapshot()
}

func (s *server) connOpened() {
	s.mu.Lock()
	s.conns++
	s.mu.Unlock()
	s.pushSnapshot()
}

func (s *server) connClosed() {
	s.mu.Lock()
	if s.conns > 0 {
		s.conns--
	}
	s.mu.Unlock()
	s.pushSnapshot()
}

func (s *server) snap() snapshot {
	s.mu.Lock()
	defer s.mu.Unlock()
	statsCopy := make(map[string]*stateStat, len(s.stats))
	for k, v := range s.stats {
		c := *v
		statsCopy[k] = &c
	}
	hist := make([]transition, len(s.history))
	copy(hist, s.history)
	return snapshot{
		Connected: s.conns > 0, Dev: s.dev, IP: s.ip,
		Current: s.current, CurConf: s.curConf,
		SinceMs: s.since.UnixMilli(), NowMs: time.Now().UnixMilli(),
		Dropped: s.dropped, Stats: statsCopy, History: hist,
	}
}

func sseFrame(event string, data []byte) []byte {
	var buf bytes.Buffer
	buf.WriteString("event: ")
	buf.WriteString(event)
	buf.WriteByte('\n')
	buf.WriteString("data: ")
	buf.Write(data)
	buf.WriteString("\n\n")
	return buf.Bytes()
}

func (s *server) pushSnapshot() {
	b, err := json.Marshal(s.snap())
	if err != nil {
		return
	}
	s.hub.broadcast(sseFrame("snap", b))
}

func (s *server) handleEvents(w http.ResponseWriter, r *http.Request) {
	fl, ok := w.(http.Flusher)
	if !ok {
		http.Error(w, "streaming unsupported", http.StatusInternalServerError)
		return
	}
	h := w.Header()
	h.Set("Content-Type", "text/event-stream")
	h.Set("Cache-Control", "no-cache")
	h.Set("Connection", "keep-alive")
	h.Set("Access-Control-Allow-Origin", "*")

	ch := s.hub.subscribe()
	defer s.hub.unsubscribe(ch)

	// Сразу отдаём текущий снимок, чтобы страница нарисовалась без ожидания события.
	if b, err := json.Marshal(s.snap()); err == nil {
		w.Write(sseFrame("snap", b))
		fl.Flush()
	}

	ping := time.NewTicker(15 * time.Second)
	defer ping.Stop()
	ctx := r.Context()
	for {
		select {
		case <-ctx.Done():
			return
		case b, ok := <-ch:
			if !ok {
				return
			}
			if _, err := w.Write(b); err != nil {
				return
			}
			fl.Flush()
		case <-ping.C:
			if _, err := w.Write([]byte(": ping\n\n")); err != nil {
				return
			}
			fl.Flush()
		}
	}
}

func (s *server) handleAPIState(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Access-Control-Allow-Origin", "*")
	json.NewEncoder(w).Encode(s.snap())
}

func main() {
	var espAddr, httpAddr string
	flag.StringVar(&espAddr, "esp", ":9000", "TCP-адрес приёма NDJSON-событий от ESP32")
	flag.StringVar(&httpAddr, "http", ":8080", "HTTP-адрес веб-морды")
	flag.Parse()

	s := newServer()

	ln, err := net.Listen("tcp", espAddr)
	if err != nil {
		log.Fatalf("listen %s: %v", espAddr, err)
	}
	go s.serveTCP(ln)

	mux := http.NewServeMux()
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/" {
			http.NotFound(w, r)
			return
		}
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.Write(indexHTML)
	})
	mux.HandleFunc("/events", s.handleEvents)
	mux.HandleFunc("/api/state", s.handleAPIState)

	log.Printf("esp-dashboard: web http://%s  | ESP events tcp %s", httpAddr, espAddr)
	if err := http.ListenAndServe(httpAddr, mux); err != nil {
		log.Fatal(err)
	}
}
