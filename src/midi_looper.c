#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/midi/midi.h>
#include <lv2/time/time.h>
#include <lv2/urid/urid.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOOPER_URI "urn:midilooper:looper"
#define PPQN 960
#define MAX_LAYERS 8
#define MAX_EVENTS 4000
#define MAX_OFFS 1024
#define MAX_SEG 1024
#define MAX_PRE 64
#define MAX_CC 16384
#define CC_GAP_SEC 0.5     /* knob pause after which CC overwriting ends */
#define MAX_TOUCH 48
#define PREROLL 360.0
#define WRAP_SNAP 240          /* ticks: an overdubbed note pressed this close to the loop end is placed at the loop start */
#define N_IN 25
#define N_OUT 11
#define OUT_BASE 27
#define VIZ_MAGIC 0x4D4C5A31u

enum { ST_EMPTY = 0, ST_ARM_REC, ST_REC, ST_ARM_STOP, ST_PLAY, ST_OVERDUB, ST_STOP, ST_ARM_PLAY };
enum { C_REC = 0, C_PLAY, C_UNDO, C_CLEAR,
       I_LAYER, C_LMUTE, C_LDEL, C_REDO,
       I_LENGTH, I_LAUNCH, C_HALF, C_DOUBLE,
       I_QGRID, I_QSTR, I_QTARGET, C_QAPPLY,
       I_ELAYER, I_TRANSP, C_EAPPLY,
       I_SLOT, C_SAVE, C_LOAD, C_SDEL,
       I_QMASK, C_EXPORT };
enum { O_STATE = 0, O_POS, O_BARS, O_LAYERS, O_MUTE, O_CONFIRM, O_BPM, O_BEAT, O_RUN, O_SLOTS, O_MSG };

typedef struct { uint32_t tick, dur; uint8_t st, d1, d2, pad; } Ev;
typedef struct { Ev* ev; uint32_t n; uint8_t muted; int8_t transp; } Layer;
typedef struct {
  uint32_t len; int nlayers;
  uint8_t muted[MAX_LAYERS]; int8_t transp[MAX_LAYERS];
  uint32_t n[MAX_LAYERS]; Ev* ev[MAX_LAYERS];
  Ev* cc; uint32_t ncc;
} Snap;
typedef struct { double clk; uint8_t ch, p; } Off;
typedef struct { uint32_t frame; uint8_t prio, st, d1, d2, final; } Out;
typedef struct { uint8_t ch, p, vel, closed; double on_clk, dur; } Pre;
typedef struct { uint16_t key; uint8_t used, active, rec; uint64_t last_sample; double last_clk; uint32_t last_tick; } Touch;
typedef struct { LV2_Atom_Event ev; uint8_t msg[3]; } MidiOut;
/* widget snapshot header (96 bytes), followed by the layer events */
typedef struct {
  uint32_t magic, seq, ver, loop_len, nlayers, bpb, state, mute_mask;
  uint32_t n[MAX_LAYERS];
  int32_t transp[MAX_LAYERS];
  uint32_t ncc, pad;
} VizHdr;

typedef struct {
  const LV2_Atom_Sequence* in;
  LV2_Atom_Sequence* out;
  uint32_t out_cap;
  const float* ctl[N_IN];
  float* outp[N_OUT];
  LV2_URID_Map* map;
  LV2_URID midi_Event, atom_Object, atom_Float, atom_Double, atom_Long, atom_Int;
  LV2_URID time_Position, time_bar, time_barBeat, time_bpm, time_speed, time_bpb;

  double sr, clk, bpm;
  int bpb;
  uint64_t samples;
  uint32_t nframes, cur_frame;
  int rolling, stop_pending, stop_has_pos;
  uint64_t stop_deadline;
  double stop_pos;

  int state;
  Layer layers[MAX_LAYERS];
  int nlayers, nredo;
  uint32_t loop_len;
  double loop_anchor, rec_start, arm_clk, auto_clk;
  int arm_kind;

  uint8_t lcnt[16][128];
  Off offs[MAX_OFFS];
  int noffs;
  Layer* open_L[16][128];
  uint32_t open_i[16][128];
  uint8_t held[16][128];
  double open_clk[16][128];
  uint8_t open_shift[16][128];
  Pre pre[MAX_PRE];
  int npre;
  Ev* cc; uint32_t ncc;
  Touch touch[MAX_TOUCH];

  float prev[N_IN];
  uint64_t rise_at[N_IN];
  int have_prev;
  uint64_t settle_until;
  int confirm;
  uint64_t confirm_until;

  Snap bk; int bk_valid;
  Snap save_s, load_s;
  int pending_load, load_kind;
  double load_clk;
  int msg_seq, msg_code;

  VizHdr* viz; size_t viz_size; int viz_fd; char viz_path[64];
  int viz_dirty; uint64_t viz_next; uint32_t viz_ver;

  pthread_t thr; sem_t sem; int thr_ok;
  atomic_int req, req_slot, quit, load_state, wmsg, slot_mask;
  char dir[256];
} Looper;

static const int bars_tbl[6] = { 0, 1, 2, 4, 8, 16 };

/* ---------- utilities ---------- */
static double pmod(double a, double b) { double r = fmod(a, b); return r < 0 ? r + b : r; }
static double tps(const Looper* L) { return L->bpm * PPQN / (60.0 * L->sr); }
static double bar_ticks(const Looper* L) { return (double)L->bpb * PPQN; }
static int is_armed(const Looper* L) {
  return L->state == ST_ARM_REC || L->state == ST_ARM_STOP || L->state == ST_ARM_PLAY;
}
static int advancing(const Looper* L) { return L->rolling && !L->stop_pending; }
static int msg_len(uint8_t st) { uint8_t t = st & 0xF0; return (t == 0xC0 || t == 0xD0) ? 2 : 3; }
static void post_msg(Looper* L, int code) { L->msg_seq = (L->msg_seq + 1) % 1000; L->msg_code = code; }

static int emit(Looper* L, uint32_t frame, uint8_t st, uint8_t d1, uint8_t d2) {
  MidiOut e;
  memset(&e, 0, sizeof e);
  if (frame >= L->nframes) frame = L->nframes ? L->nframes - 1 : 0;
  e.ev.time.frames = frame;
  e.ev.body.type = L->midi_Event;
  e.ev.body.size = msg_len(st);
  e.msg[0] = st; e.msg[1] = d1; e.msg[2] = d2;
  return lv2_atom_sequence_append_event(L->out, L->out_cap, &e.ev) != NULL;
}

static void silence_loop(Looper* L) {
  for (int c = 0; c < 16; c++)
    for (int p = 0; p < 128; p++)
      if (L->lcnt[c][p] && emit(L, L->cur_frame, 0x80 | c, p, 0)) L->lcnt[c][p] = 0;
  L->noffs = 0;
}

static double next_boundary(const Looper* L, int kind, int inclusive) {
  if (kind == 3 && L->loop_len > 0) {
    double r = pmod(L->clk - L->loop_anchor, (double)L->loop_len);
    if (inclusive && r < 1e-6) return L->clk;
    return L->clk + ((double)L->loop_len - r);
  }
  double u = (kind == 1) ? (double)PPQN : bar_ticks(L);
  double r = pmod(L->clk, u);
  if (inclusive && r < 1e-6) return L->clk;
  return L->clk + (u - r);
}

/* ---------- widget snapshot (piano roll) ---------- */
static void viz_export(Looper* L) {
  if (!L->viz) return;
  VizHdr* h = L->viz;
  Ev* dst = (Ev*)(h + 1);
  uint32_t s = h->seq;
  __atomic_store_n(&h->seq, s + 1, __ATOMIC_RELEASE);
  h->loop_len = L->loop_len; h->nlayers = (uint32_t)L->nlayers; h->bpb = (uint32_t)L->bpb;
  h->state = (uint32_t)L->state;
  uint32_t mask = 0, off = 0;
  for (int i = 0; i < MAX_LAYERS; i++) {
    if (i < L->nlayers) {
      uint32_t n = L->layers[i].n;
      h->n[i] = n; h->transp[i] = L->layers[i].transp;
      if (L->layers[i].muted) mask |= (1u << i);
      memcpy(dst + off, L->layers[i].ev, n * sizeof(Ev));
      off += n;
    } else { h->n[i] = 0; h->transp[i] = 0; }
  }
  h->mute_mask = mask;
  memcpy(dst + off, L->cc, L->ncc * sizeof(Ev));
  h->ncc = L->ncc;
  h->ver = ++L->viz_ver;
  __atomic_store_n(&h->seq, s + 2, __ATOMIC_RELEASE);
}

/* ---------- data snapshots (backup, slots) ---------- */
static int snap_alloc(Snap* s) {
  for (int i = 0; i < MAX_LAYERS; i++) {
    s->ev[i] = (Ev*)calloc(MAX_EVENTS, sizeof(Ev));
    if (!s->ev[i]) return 0;
  }
  s->cc = (Ev*)calloc(MAX_CC, sizeof(Ev));
  return s->cc != NULL;
}
static void snap_free(Snap* s) {
  for (int i = 0; i < MAX_LAYERS; i++) { free(s->ev[i]); s->ev[i] = NULL; }
  free(s->cc); s->cc = NULL;
}

static void snap_from_layers(const Looper* L, Snap* s) {
  s->len = L->loop_len; s->nlayers = L->nlayers;
  s->ncc = L->ncc; memcpy(s->cc, L->cc, L->ncc * sizeof(Ev));
  for (int i = 0; i < L->nlayers; i++) {
    s->muted[i] = L->layers[i].muted; s->transp[i] = L->layers[i].transp; s->n[i] = L->layers[i].n;
    memcpy(s->ev[i], L->layers[i].ev, L->layers[i].n * sizeof(Ev));
  }
}
static void snap_to_layers(Looper* L, const Snap* s) {
  for (int i = 0; i < MAX_LAYERS; i++) { L->layers[i].n = 0; L->layers[i].muted = 0; L->layers[i].transp = 0; }
  for (int i = 0; i < s->nlayers; i++) {
    L->layers[i].n = s->n[i]; L->layers[i].muted = s->muted[i]; L->layers[i].transp = s->transp[i];
    memcpy(L->layers[i].ev, s->ev[i], s->n[i] * sizeof(Ev));
  }
  L->nlayers = s->nlayers; L->loop_len = s->len;
  L->ncc = s->ncc; memcpy(L->cc, s->cc, s->ncc * sizeof(Ev));
}
static void backup_make(Looper* L) { snap_from_layers(L, &L->bk); L->bk_valid = 1; }

/* ---------- slot files (worker thread) ---------- */
static void slot_path(const Looper* L, int slot, char* out, size_t sz) {
  snprintf(out, sz, "%s/slot%02d.mlp", L->dir, slot);
}
static void scan_slots(Looper* L) {
  int m = 0; char p[320];
  for (int s = 1; s <= 16; s++) { slot_path(L, s, p, sizeof p); if (access(p, F_OK) == 0) m |= 1 << (s - 1); }
  atomic_store(&L->slot_mask, m);
}
static int file_save(Looper* L, int slot) {
  char p[320], tmp[340];
  mkdir(L->dir, 0755);
  slot_path(L, slot, p, sizeof p);
  snprintf(tmp, sizeof tmp, "%s.tmp", p);
  FILE* f = fopen(tmp, "wb");
  if (!f) return 0;
  Snap* s = &L->save_s;
  uint32_t magic = 0x4D4C5033u, len = s->len, nl = (uint32_t)s->nlayers;
  fwrite(&magic, 4, 1, f); fwrite(&len, 4, 1, f); fwrite(&nl, 4, 1, f);
  for (uint32_t i = 0; i < nl; i++) {
    uint32_t mu = (s->muted[i] ? 1u : 0u) | ((uint32_t)(uint8_t)s->transp[i] << 8), n = s->n[i];
    fwrite(&mu, 4, 1, f); fwrite(&n, 4, 1, f);
    fwrite(s->ev[i], sizeof(Ev), n, f);
  }
  { uint32_t nc = s->ncc; fwrite(&nc, 4, 1, f); fwrite(s->cc, sizeof(Ev), nc, f); }
  int ok = (fclose(f) == 0);
  if (ok) ok = (rename(tmp, p) == 0); else remove(tmp);
  return ok;
}
static int file_load(Looper* L, int slot) {
  char p[320];
  slot_path(L, slot, p, sizeof p);
  FILE* f = fopen(p, "rb");
  if (!f) return 0;
  Snap* s = &L->load_s;
  uint32_t magic = 0, len = 0, nl = 0;
  int ok = (fread(&magic, 4, 1, f) == 1 && fread(&len, 4, 1, f) == 1 && fread(&nl, 4, 1, f) == 1);
  if (ok && ((magic != 0x4D4C5032u && magic != 0x4D4C5033u) || len < 1 || nl < 1 || nl > MAX_LAYERS)) ok = 0;
  for (uint32_t i = 0; ok && i < nl; i++) {
    uint32_t mu = 0, n = 0;
    if (fread(&mu, 4, 1, f) != 1 || fread(&n, 4, 1, f) != 1 || n > MAX_EVENTS) { ok = 0; break; }
    if (fread(s->ev[i], sizeof(Ev), n, f) != n) { ok = 0; break; }
    s->muted[i] = (mu & 1) ? 1 : 0; s->transp[i] = (int8_t)((mu >> 8) & 0xFF); s->n[i] = n;
  }
  s->ncc = 0;
  if (ok && magic == 0x4D4C5033u) {
    uint32_t nc = 0;
    if (fread(&nc, 4, 1, f) != 1 || nc > MAX_CC || fread(s->cc, sizeof(Ev), nc, f) != nc) ok = 0;
    else s->ncc = nc;
  }
  fclose(f);
  if (ok) { s->len = len; s->nlayers = (int)nl; }
  return ok;
}
static void* worker_main(void* arg) {
  Looper* L = (Looper*)arg;
  for (;;) {
    while (sem_wait(&L->sem) != 0) {}
    if (atomic_load(&L->quit)) break;
    int req = atomic_load(&L->req), slot = atomic_load(&L->req_slot);
    if (req == 1) atomic_store(&L->wmsg, file_save(L, slot) ? 1 : 5);
    else if (req == 2) atomic_store(&L->load_state, file_load(L, slot) ? 1 : 2);
    else if (req == 3) { char p[320]; slot_path(L, slot, p, sizeof p); remove(p); atomic_store(&L->wmsg, 3); }
    scan_slots(L);
    atomic_store(&L->req, 0);
  }
  return NULL;
}
static void worker_request(Looper* L, int req, int slot) {
  atomic_store(&L->req_slot, slot);
  atomic_store(&L->req, req);
  sem_post(&L->sem);
}

/* ---------- recording ---------- */
static uint32_t rec_tick(const Looper* L) {
  if (L->state == ST_REC || L->state == ST_ARM_STOP) {
    double d = L->clk - L->rec_start;
    return d < 0 ? 0 : (uint32_t)(d + 0.5);
  }
  if (L->loop_len > 0) return (uint32_t)pmod(L->clk - L->loop_anchor, (double)L->loop_len);
  return 0;
}
static Ev* push_ev(Looper* L, uint8_t st, uint8_t d1, uint8_t d2, uint32_t* idx) {
  if (L->nlayers < 1) return NULL;
  Layer* ly = &L->layers[L->nlayers - 1];
  if (ly->n >= MAX_EVENTS) return NULL;
  Ev* e = &ly->ev[ly->n];
  e->tick = rec_tick(L); e->dur = 0; e->st = st; e->d1 = d1; e->d2 = d2; e->pad = 0;
  if (idx) *idx = ly->n;
  ly->n++;
  L->viz_dirty = 1;
  return e;
}
static void close_note(Looper* L, int ch, int p) {
  Layer* ly = L->open_L[ch][p];
  if (!ly) return;
  Ev* e = &ly->ev[L->open_i[ch][p]];
  uint32_t now = rec_tick(L), dur;
  if (L->open_shift[ch][p]) {                      /* note moved to the loop start: use the real duration */
    double rd = L->clk - L->open_clk[ch][p];
    dur = rd < 1 ? 1 : (uint32_t)(rd + 0.5);
    L->open_shift[ch][p] = 0;
  } else if (now >= e->tick) dur = now - e->tick;
  else dur = now + L->loop_len - e->tick;
  if (dur < 1) dur = 1;
  if (L->loop_len > 1 && dur >= L->loop_len) dur = L->loop_len - 1;
  e->dur = dur;
  L->open_L[ch][p] = NULL;
  L->viz_dirty = 1;
}
static void close_all_open_at_end(Looper* L) {
  for (int c = 0; c < 16; c++)
    for (int p = 0; p < 128; p++) {
      Layer* ly = L->open_L[c][p];
      if (!ly) continue;
      Ev* e = &ly->ev[L->open_i[c][p]];
      e->dur = (L->loop_len > e->tick) ? L->loop_len - e->tick : 1;
      L->open_L[c][p] = NULL;
    }
  L->viz_dirty = 1;
}
static void drop_all_open(Looper* L) { memset(L->open_L, 0, sizeof L->open_L); }
static void rec_note_on(Looper* L, int ch, int p, int vel) {
  if (L->open_L[ch][p]) close_note(L, ch, p);
  uint32_t i;
  Ev* e = push_ev(L, 0x90 | ch, p, vel, &i);
  if (!e) return;
  L->open_L[ch][p] = &L->layers[L->nlayers - 1]; L->open_i[ch][p] = i;
  L->open_shift[ch][p] = 0;
  if (L->state == ST_OVERDUB && L->loop_len > 2 * WRAP_SNAP && e->tick + WRAP_SNAP >= L->loop_len) {
    e->tick = 0;                                   /* instead of a micro-note at the end: the note goes to the loop start */
    L->open_shift[ch][p] = 1; L->open_clk[ch][p] = L->clk;
  }
}

/* ---------- CC, pitch bend, aftertouch: shared lane (not per layer) ---------- */
static uint16_t cc_key(uint8_t st, uint8_t d1) {
  uint8_t type = st & 0xF0;
  return (uint16_t)(((uint16_t)st << 8) | ((type == 0xB0 || type == 0xA0) ? d1 : 0));
}
static Touch* touch_get(Looper* L, uint16_t key) {
  Touch* fr = NULL;
  Touch* oldest = &L->touch[0];
  for (int i = 0; i < MAX_TOUCH; i++) {
    Touch* t = &L->touch[i];
    if (t->used && t->key == key) return t;
    if (!t->used && !fr) fr = t;
    if (t->last_sample < oldest->last_sample) oldest = t;
  }
  Touch* t = fr ? fr : oldest;
  memset(t, 0, sizeof *t);
  t->used = 1; t->key = key;
  return t;
}
static int cc_touched(const Looper* L, const Ev* e) {
  uint16_t key = cc_key(e->st, e->d1);
  const int64_t gap = (int64_t)(CC_GAP_SEC * L->sr);
  for (int i = 0; i < MAX_TOUCH; i++) {
    const Touch* t = &L->touch[i];
    if (t->used && t->key == key) {
      int64_t d = (int64_t)L->samples - (int64_t)t->last_sample;
      return t->active && d <= gap;
    }
  }
  return 0;
}
static void cc_erase(Looper* L, uint16_t key, uint32_t start, uint32_t count) {
  if (L->loop_len == 0 || count == 0) return;
  if (count > L->loop_len) count = L->loop_len;
  start %= L->loop_len;
  uint32_t k = 0;
  for (uint32_t i = 0; i < L->ncc; i++) {
    const Ev* e = &L->cc[i];
    if (cc_key(e->st, e->d1) == key) {
      uint32_t rel = (e->tick + L->loop_len - start) % L->loop_len;
      if (rel < count) continue;
    }
    if (k != i) L->cc[k] = L->cc[i];
    k++;
  }
  L->ncc = k;
}
static void cc_put(Looper* L, uint16_t key, uint8_t st, uint8_t d1, uint8_t d2, uint32_t tick) {
  uint32_t lim = L->ncc > 64 ? L->ncc - 64 : 0;
  for (uint32_t i = L->ncc; i > lim; i--) {
    Ev* e = &L->cc[i - 1];
    if (e->tick == tick && cc_key(e->st, e->d1) == key) { e->d1 = d1; e->d2 = d2; return; }
  }
  if (L->ncc >= MAX_CC) return;
  Ev* e = &L->cc[L->ncc++];
  e->tick = tick; e->dur = 0; e->st = st; e->d1 = d1; e->d2 = d2; e->pad = 0;
}
/* A controller value arrived: touching the knob mutes its playback from the loop,
   and in REC/OVERDUB it also overwrites the lane; a pause > CC_GAP_SEC = the knob was released. */
static void cc_event(Looper* L, uint8_t st, uint8_t d1, uint8_t d2) {
  const uint16_t key = cc_key(st, d1);
  Touch* t = touch_get(L, key);
  const int64_t gap = (int64_t)(CC_GAP_SEC * L->sr);
  const uint64_t now = L->samples + L->cur_frame;
  int fresh = !t->active || (int64_t)(now - t->last_sample) > gap;
  int recording = advancing(L) &&
                  (L->state == ST_REC || L->state == ST_ARM_STOP || L->state == ST_OVERDUB);
  if (fresh) t->rec = 0;
  t->active = 1;
  if (recording) {
    uint32_t pos = rec_tick(L);
    if (L->state == ST_OVERDUB && L->loop_len > 0) {
      if (!t->rec) cc_erase(L, key, pos, 1);
      else {
        double dt = L->clk - t->last_clk;
        uint32_t n = dt > 0 ? (uint32_t)(dt + 0.5) : 0;
        cc_erase(L, key, (t->last_tick + 1) % L->loop_len, n);
      }
    }
    cc_put(L, key, st, d1, d2, pos);
    t->rec = 1; t->last_tick = pos; t->last_clk = L->clk;
    L->viz_dirty = 1;
  } else {
    t->rec = 0;
  }
  t->last_sample = now;
}

/* ---------- states ---------- */
static void reset_layers(Looper* L) {
  for (int i = 0; i < MAX_LAYERS; i++) { L->layers[i].n = 0; L->layers[i].muted = 0; L->layers[i].transp = 0; }
}

static void do_clear(Looper* L) {
  silence_loop(L);
  reset_layers(L);
  L->nlayers = 0; L->nredo = 0; L->loop_len = 0;
  L->state = ST_EMPTY; L->arm_clk = -1; L->auto_clk = -1;
  L->confirm = 0; L->npre = 0; L->bk_valid = 0;
  L->ncc = 0; memset(L->touch, 0, sizeof L->touch);
  L->pending_load = 0; atomic_store(&L->load_state, 0);
  drop_all_open(L);
  L->viz_dirty = 1;
}

static void begin_rec(Looper* L, double at) {
  reset_layers(L);
  L->nlayers = 1; L->nredo = 0; L->loop_len = 0; L->bk_valid = 0;
  L->ncc = 0; memset(L->touch, 0, sizeof L->touch);
  L->pending_load = 0; atomic_store(&L->load_state, 0);
  L->rec_start = at; L->state = ST_REC; L->arm_clk = -1;
  int li = (int)(*L->ctl[I_LENGTH] + 0.5f);
  if (li < 0) li = 0;
  if (li > 5) li = 5;
  int nb = bars_tbl[li];
  L->auto_clk = nb > 0 ? at + nb * bar_ticks(L) : -1;
  drop_all_open(L);
  for (int k = 0; k < L->npre; k++) {
    Pre* q = &L->pre[k];
    uint32_t i;
    Ev* e = push_ev(L, 0x90 | q->ch, q->p, q->vel, &i);
    if (!e) break;
    e->tick = 0;
    if (q->closed) { e->dur = (q->dur < 1) ? 1 : (uint32_t)(q->dur + 0.5); }
    else { L->open_L[q->ch][q->p] = &L->layers[0]; L->open_i[q->ch][q->p] = i; }
  }
  L->npre = 0;
  for (int c = 0; c < 16; c++)
    for (int p = 0; p < 128; p++)
      if (L->held[c][p] && !L->open_L[c][p]) rec_note_on(L, c, p, L->held[c][p]);
}

static void finish_rec(Looper* L, double at) {
  uint32_t len = (uint32_t)(at - L->rec_start + 0.5);
  L->arm_clk = -1; L->auto_clk = -1;
  if (len < 1) { do_clear(L); return; }
  L->loop_len = len; L->loop_anchor = L->rec_start;
  Layer* ly = &L->layers[0];
  for (uint32_t j = 0; j < ly->n; j++)
    if (ly->ev[j].tick >= len) ly->ev[j].tick = len - 1;
  for (uint32_t j = 0; j < L->ncc; j++) if (L->cc[j].tick >= len) L->cc[j].tick = len - 1;
  close_all_open_at_end(L);
  L->state = ST_PLAY;
}

static void apply_load(Looper* L, double at) {
  silence_loop(L);
  drop_all_open(L);
  snap_to_layers(L, &L->load_s);
  L->nredo = 0; L->loop_anchor = at; L->state = ST_PLAY;
  L->arm_clk = -1; L->auto_clk = -1; L->bk_valid = 0;
  L->pending_load = 0; atomic_store(&L->load_state, 0);
  L->viz_dirty = 1;
  post_msg(L, 2);
}

static void fire_action(Looper* L, double at) {
  int s = L->state;
  L->arm_clk = -1;
  if (s == ST_ARM_REC) begin_rec(L, at);
  else if (s == ST_ARM_STOP || s == ST_REC) finish_rec(L, at);
  else if (s == ST_ARM_PLAY) L->state = ST_PLAY;
  L->viz_dirty = 1;
}

static void arm_or_fire(Looper* L, int armstate) {
  int kind = (int)(*L->ctl[I_LAUNCH] + 0.5f);
  if (kind < 1) kind = 2;
  if (kind > 3) kind = 3;
  L->state = armstate;
  L->arm_kind = kind;
  L->arm_clk = next_boundary(L, kind, !L->rolling);
  if (armstate == ST_ARM_REC) L->npre = 0;
}

/* ---------- loop edits ---------- */
static int editable(const Looper* L) {
  return (L->state == ST_PLAY || L->state == ST_STOP) && L->loop_len > 0 && L->nlayers > 0;
}

static void edit_half(Looper* L) {
  if (!editable(L) || L->loop_len < 2 * PPQN) { post_msg(L, 10); return; }
  backup_make(L); silence_loop(L); drop_all_open(L);
  uint32_t nl = L->loop_len / 2;
  for (int i = 0; i < L->nlayers; i++) {
    Layer* ly = &L->layers[i];
    uint32_t k = 0;
    for (uint32_t j = 0; j < ly->n; j++) {
      Ev e = ly->ev[j];
      if (e.tick >= nl) continue;
      if ((e.st & 0xF0) == 0x90 && e.dur >= nl) e.dur = nl - 1;
      ly->ev[k++] = e;
    }
    ly->n = k;
  }
  { uint32_t k = 0; for (uint32_t j = 0; j < L->ncc; j++) if (L->cc[j].tick < nl) L->cc[k++] = L->cc[j]; L->ncc = k; }
  L->loop_len = nl;
  post_msg(L, 8);
}

static void edit_double(Looper* L) {
  if (!editable(L) || L->loop_len > (1u << 19)) { post_msg(L, 10); return; }
  for (int i = 0; i < L->nlayers; i++)
    if (L->layers[i].n * 2 > MAX_EVENTS) { post_msg(L, 10); return; }
  if (L->ncc * 2 > MAX_CC) { post_msg(L, 10); return; }
  backup_make(L); silence_loop(L); drop_all_open(L);
  for (int i = 0; i < L->nlayers; i++) {
    Layer* ly = &L->layers[i];
    for (uint32_t j = 0; j < ly->n; j++) {
      ly->ev[ly->n + j] = ly->ev[j];
      ly->ev[ly->n + j].tick += L->loop_len;
    }
    ly->n *= 2;
  }
  for (uint32_t j = 0; j < L->ncc; j++) { L->cc[L->ncc + j] = L->cc[j]; L->cc[L->ncc + j].tick += L->loop_len; }
  L->ncc *= 2;
  L->loop_len *= 2;
  post_msg(L, 9);
}

static void edit_quantize(Looper* L) {
  static const double units[5] = { 0, 960, 480, 240, 120 };
  int g = (int)(*L->ctl[I_QGRID] + 0.5f), str = (int)(*L->ctl[I_QSTR] + 0.5f);
  int tg = (int)(*L->ctl[I_QTARGET] + 0.5f);
  if (g < 1 || g > 4 || str <= 0) { post_msg(L, 14); return; }
  if (!editable(L)) { post_msg(L, 6); return; }
  int qmask = (int)(*L->ctl[I_QMASK] + 0.5f), qany = 0;
  for (int i = 0; i < L->nlayers; i++) if ((qmask >> i) & 1) qany = 1;
  if (!qany) { post_msg(L, 17); return; }
  backup_make(L); silence_loop(L); drop_all_open(L);
  const double u = units[g], k = str / 100.0, len = (double)L->loop_len;
  for (int i = 0; i < L->nlayers; i++) {
    Layer* ly = &L->layers[i];
    for (uint32_t j = 0; j < ly->n; j++) {
      if (!((qmask >> i) & 1)) continue;
      Ev* e = &ly->ev[j];
      if ((e->st & 0xF0) != 0x90 || e->dur == 0) continue;
      double s = e->tick, en = s + e->dur, s2 = s, e2;
      if (tg == 0 || tg == 2) { double nr = floor(s / u + 0.5) * u; s2 = s + (nr - s) * k; }
      if (tg == 0) e2 = s2 + e->dur;
      else { double nr = floor(en / u + 0.5) * u; e2 = en + (nr - en) * k; }
      double d = e2 - s2;
      if (d < 1) d = (tg == 0) ? 1 : u;
      if (len > 1 && d >= len) d = len - 1;
      double ns = floor(s2 + 0.5);
      if (ns >= len) ns -= len;
      if (ns < 0) ns = 0;
      e->tick = (uint32_t)ns; e->dur = (uint32_t)(d + 0.5);
    }
  }
  post_msg(L, 7);
}

/* ---------- slots ---------- */
static int cur_slot(const Looper* L) { return (int)(*L->ctl[I_SLOT] + 0.5f); }

static void request_save(Looper* L) {
  int slot = cur_slot(L);
  if (slot < 1 || slot > 16) { post_msg(L, 12); return; }
  if (L->loop_len == 0 || L->nlayers == 0 ||
      !(L->state == ST_PLAY || L->state == ST_STOP || L->state == ST_OVERDUB)) { post_msg(L, 6); return; }
  if (!L->thr_ok || atomic_load(&L->req) != 0) { post_msg(L, 15); return; }
  snap_from_layers(L, &L->save_s);
  worker_request(L, 1, slot);
}

static void request_load(Looper* L, int slot) {
  if (slot < 1 || slot > 16) { post_msg(L, 12); return; }
  if (!(L->state == ST_EMPTY || L->state == ST_PLAY || L->state == ST_STOP || L->state == ST_OVERDUB)) {
    post_msg(L, 15); return;
  }
  if (!(atomic_load(&L->slot_mask) & (1 << (slot - 1)))) { post_msg(L, 4); return; }
  if (!L->thr_ok || atomic_load(&L->req) != 0 || atomic_load(&L->load_state) != 0 || L->pending_load) {
    post_msg(L, 15); return;
  }
  worker_request(L, 2, slot);
}

static void request_delete(Looper* L, int slot) {
  if (!L->thr_ok || atomic_load(&L->req) != 0) { post_msg(L, 15); return; }
  worker_request(L, 3, slot);
}

static void poll_worker(Looper* L) {
  int wm = atomic_exchange(&L->wmsg, 0);
  if (wm) post_msg(L, wm);
  int ls = atomic_load(&L->load_state);
  if (ls == 2) { atomic_store(&L->load_state, 0); post_msg(L, 4); }
  else if (ls == 1 && !L->pending_load) {
    L->pending_load = 1;
    L->load_kind = (L->loop_len > 0 && (L->state == ST_PLAY || L->state == ST_OVERDUB || L->state == ST_STOP)) ? 3 : 2;
    L->load_clk = next_boundary(L, L->load_kind, !L->rolling);
    post_msg(L, 13);
  }
}

/* ---------- commands ---------- */
static int confirm_cmd(Looper* L, int code) {
  if (L->confirm == code && L->samples < L->confirm_until) { L->confirm = 0; return 1; }
  L->confirm = code;
  L->confirm_until = L->samples + (uint64_t)(3.0 * L->sr);
  return 0;
}

static int sel_from(const Looper* L, int ctl) {
  int s = (int)(*L->ctl[ctl] + 0.5f) - 1;
  if (s >= L->nlayers) s = L->nlayers - 1;
  if (s < 0) s = 0;
  return s;
}

static void run_cmd(Looper* L, int cmd) {
  if (cmd != C_CLEAR && cmd != C_LDEL && cmd != C_SDEL) L->confirm = 0;
  L->viz_dirty = 1;
  switch (cmd) {
    case C_REC:
      switch (L->state) {
        case ST_EMPTY: arm_or_fire(L, ST_ARM_REC); break;
        case ST_ARM_REC: L->state = ST_EMPTY; L->arm_clk = -1; L->npre = 0; break;
        case ST_REC: if (L->auto_clk < 0) arm_or_fire(L, ST_ARM_STOP); break;
        case ST_PLAY:
          if (L->nlayers < MAX_LAYERS) {
            Layer* ly = &L->layers[L->nlayers];
            ly->n = 0; ly->muted = 0; ly->transp = 0;
            L->nlayers++; L->nredo = 0; L->bk_valid = 0; L->state = ST_OVERDUB;
          }
          break;
        case ST_OVERDUB: L->state = ST_PLAY; break;
        default: break;
      }
      break;
    case C_PLAY:
      if (L->state == ST_PLAY || L->state == ST_OVERDUB) { silence_loop(L); L->state = ST_STOP; }
      else if (L->state == ST_STOP) arm_or_fire(L, ST_ARM_PLAY);
      else if (L->state == ST_ARM_PLAY) { L->state = ST_STOP; L->arm_clk = -1; }
      break;
    case C_UNDO:
      if (L->bk_valid && (L->state == ST_PLAY || L->state == ST_STOP)) {
        silence_loop(L); drop_all_open(L);
        snap_to_layers(L, &L->bk); L->bk_valid = 0; L->nredo = 0;
        post_msg(L, 11);
      } else if (L->state == ST_OVERDUB) {
        drop_all_open(L); silence_loop(L);
        L->nlayers--; L->nredo++; L->state = ST_PLAY;
      } else if ((L->state == ST_PLAY || L->state == ST_STOP) && L->nlayers > 1) {
        silence_loop(L); L->nlayers--; L->nredo++;
      }
      break;
    case C_REDO:
      if ((L->state == ST_PLAY || L->state == ST_STOP) && L->nredo > 0 && L->nlayers < MAX_LAYERS) {
        L->nlayers++; L->nredo--;
      }
      break;
    case C_CLEAR:
      if (L->state != ST_EMPTY && confirm_cmd(L, 1)) do_clear(L);
      break;
    case C_LMUTE:
      if (L->nlayers > 0 && L->state != ST_REC && L->state != ST_ARM_STOP) {
        int s = sel_from(L, I_LAYER);
        L->layers[s].muted ^= 1;
        silence_loop(L);
      }
      break;
    case C_LDEL:
      if ((L->state == ST_PLAY || L->state == ST_STOP) && L->nlayers > 0 && confirm_cmd(L, 2)) {
        int s = sel_from(L, I_LAYER);
        Layer tmp = L->layers[s];
        for (int k = s; k < MAX_LAYERS - 1; k++) L->layers[k] = L->layers[k + 1];
        tmp.n = 0; tmp.muted = 0; tmp.transp = 0;
        L->layers[MAX_LAYERS - 1] = tmp;
        L->nlayers--; L->nredo = 0; L->bk_valid = 0;
        drop_all_open(L); silence_loop(L);
        if (L->nlayers == 0) do_clear(L);
      }
      break;
    case C_HALF:   edit_half(L); break;
    case C_DOUBLE: edit_double(L); break;
    case C_QAPPLY: edit_quantize(L); break;
    case C_EAPPLY:
      if (L->nlayers > 0 && (L->state == ST_PLAY || L->state == ST_STOP || L->state == ST_OVERDUB)) {
        int s = sel_from(L, I_ELAYER);
        int t = (int)floor(*L->ctl[I_TRANSP] + 0.5);
        if (t == 0) { post_msg(L, 18); break; }
        int nv = L->layers[s].transp + t;
        if (nv < -48) nv = -48;
        if (nv > 48) nv = 48;
        L->layers[s].transp = (int8_t)nv;
        silence_loop(L);
        post_msg(L, 16);
      } else post_msg(L, 6);
      break;
    case C_EXPORT:
      if (L->loop_len > 0 && L->nlayers > 0 &&
          (L->state == ST_PLAY || L->state == ST_STOP || L->state == ST_OVERDUB)) post_msg(L, 19);
      else post_msg(L, 6);
      break;
    case C_SAVE:   request_save(L); break;
    case C_LOAD:   request_load(L, cur_slot(L)); break;
    case C_SDEL: {
      int slot = cur_slot(L);
      if (slot < 1 || slot > 16) post_msg(L, 12);
      else if (confirm_cmd(L, 3)) request_delete(L, slot);
      break;
    }
  }
}

static int is_momentary(int i) { return i == C_HALF || i == C_DOUBLE || i == C_QAPPLY || i == C_EAPPLY || i == C_EXPORT; }

static void process_controls(Looper* L) {
  float cur[N_IN];
  for (int i = 0; i < N_IN; i++) cur[i] = *L->ctl[i];
  if (!L->have_prev) { memcpy(L->prev, cur, sizeof cur); L->have_prev = 1; return; }
  int changed = 0;
  for (int i = 0; i < N_IN; i++) if (cur[i] != L->prev[i]) changed++;
  int restore = (changed >= 2) || (L->samples < L->settle_until);
  if (changed >= 2) L->settle_until = L->samples + (uint64_t)(0.2 * L->sr);
  if (restore && cur[I_SLOT] != L->prev[I_SLOT] && cur[I_SLOT] >= 1)
    request_load(L, (int)(cur[I_SLOT] + 0.5f));
  if (!restore && changed > 0) {
    static const int cmds[15] = { C_REC, C_PLAY, C_UNDO, C_CLEAR, C_LMUTE, C_LDEL, C_REDO,
                                  C_HALF, C_DOUBLE, C_QAPPLY, C_EAPPLY, C_SAVE, C_LOAD, C_SDEL, C_EXPORT };
    for (int k = 0; k < 15; k++) {
      int i = cmds[k];
      int was = L->prev[i] >= 0.5f, now = cur[i] >= 0.5f;
      if (was == now) continue;
      int press;
      if (!is_momentary(i)) press = 1;
      else if (now) { press = 1; L->rise_at[i] = L->samples; }                       /* press */
      else press = (L->samples - L->rise_at[i]) > (uint64_t)(1.0 * L->sr);           /* ignore the release if < 1 s */
      if (press) run_cmd(L, i);
    }
  }
  memcpy(L->prev, cur, sizeof cur);
}

/* ---------- playback ---------- */
static void play_segment(Looper* L, uint32_t from, uint32_t to) {
  if (to <= from) return;
  const double t = tps(L), span = (to - from) * t, clk0 = L->clk;
  Out buf[MAX_SEG];
  int nb = 0;
#define ADD(fr, pr, s, a, b) do { if (nb < MAX_SEG) { buf[nb].frame = (fr); buf[nb].prio = (pr); \
  buf[nb].st = (s); buf[nb].d1 = (a); buf[nb].d2 = (b); buf[nb].final = 0; nb++; } } while (0)

  if ((L->state == ST_PLAY || L->state == ST_OVERDUB) && L->loop_len > 0) {
    const double len = L->loop_len;
    const double rel = pmod(clk0 - L->loop_anchor, len);
    double wa[2], wb[2], wc[2];
    int nw;
    if (rel + span <= len) { wa[0] = rel; wb[0] = rel + span; wc[0] = 0; nw = 1; }
    else {
      wa[0] = rel; wb[0] = len; wc[0] = 0;
      wa[1] = 0; wb[1] = rel + span - len; wc[1] = len - rel; nw = 2;
    }
    for (int i = 0; i < L->nlayers; i++) {
      Layer* ly = &L->layers[i];
      if (ly->muted) continue;
      const int tr = ly->transp;
      for (uint32_t j = 0; j < ly->n; j++) {
        const Ev* e = &ly->ev[j];
        int isnote = ((e->st & 0xF0) == 0x90);
        if (isnote && e->dur == 0) continue;
        for (int w = 0; w < nw; w++) {
          if ((double)e->tick >= wa[w] && (double)e->tick < wb[w]) {
            double off = wc[w] + ((double)e->tick - wa[w]);
            uint32_t fr = from + (uint32_t)(off / t);
            if (fr >= to) fr = to - 1;
            if (isnote) {
              int c = e->st & 0x0F, p = e->d1 + tr;
              if (p < 0) p = 0;
              if (p > 127) p = 127;
              if (L->noffs < MAX_OFFS) {
                if (L->lcnt[c][p]) ADD(fr, 0, 0x80 | c, p, 0);
                if (L->lcnt[c][p] < 250) L->lcnt[c][p]++;
                ADD(fr, 1, e->st, p, e->d2);
                L->offs[L->noffs].clk = clk0 + off + (double)(e->dur ? e->dur : 1);
                L->offs[L->noffs].ch = c; L->offs[L->noffs].p = p;
                L->noffs++;
              }
            } else {
              ADD(fr, 1, e->st, e->d1, e->d2);
            }
            break;
          }
        }
      }
    }
    for (uint32_t j = 0; j < L->ncc; j++) {
      const Ev* e = &L->cc[j];
      for (int w = 0; w < nw; w++) {
        if ((double)e->tick >= wa[w] && (double)e->tick < wb[w]) {
          if (!cc_touched(L, e)) {
            double off = wc[w] + ((double)e->tick - wa[w]);
            uint32_t fr = from + (uint32_t)(off / t);
            if (fr >= to) fr = to - 1;
            ADD(fr, 1, e->st, e->d1, e->d2);
          }
          break;
        }
      }
    }
  }
  for (int i = 0; i < L->noffs;) {
    if (L->offs[i].clk < clk0 + span) {
      double d = (L->offs[i].clk - clk0) / t;
      uint32_t fr = from + (d > 0 ? (uint32_t)d : 0);
      if (fr >= to) fr = to - 1;
      uint8_t c = L->offs[i].ch, p = L->offs[i].p;
      if (L->lcnt[c][p]) {
        L->lcnt[c][p]--;
        if (!L->lcnt[c][p]) {
          int before = nb;
          ADD(fr, 0, 0x80 | c, p, 0);
          if (nb > before) buf[nb - 1].final = 1;
          else L->lcnt[c][p] = 1;                 /* retried by reconcile_notes() */
        }
      }
      L->offs[i] = L->offs[--L->noffs];
    } else i++;
  }
  for (int i = 1; i < nb; i++) {
    Out k = buf[i];
    int j = i - 1;
    while (j >= 0 && (buf[j].frame > k.frame || (buf[j].frame == k.frame && buf[j].prio > k.prio))) {
      buf[j + 1] = buf[j]; j--;
    }
    buf[j + 1] = k;
  }
  for (int i = 0; i < nb; i++) {
    if (!emit(L, buf[i].frame, buf[i].st, buf[i].d1, buf[i].d2) && buf[i].final)
      L->lcnt[buf[i].st & 0x0F][buf[i].d1] = 1;   /* output buffer was full: the note-off is retried later */
  }
#undef ADD
}

static void advance_to(Looper* L, uint32_t target) {
  if (!advancing(L)) { if (L->cur_frame < target) L->cur_frame = target; return; }
  while (L->cur_frame < target) {
    const double t = tps(L);
    uint32_t seg_end = target;
    int fire = 0, ck = 0;
    double cand = -1;
    if (is_armed(L)) { cand = L->arm_clk; ck = 1; }
    if (L->state == ST_REC && L->auto_clk > 0 && (cand < 0 || L->auto_clk < cand)) { cand = L->auto_clk; ck = 1; }
    if (L->pending_load && (cand < 0 || L->load_clk < cand)) { cand = L->load_clk; ck = 2; }
    if (cand >= 0) {
      double d = (cand - L->clk) / t;
      if (d < 0) d = 0;
      uint32_t fr = L->cur_frame + (uint32_t)ceil(d);
      if (fr < target || (fr == target && target < L->nframes)) { seg_end = fr; fire = 1; }
    }
    play_segment(L, L->cur_frame, seg_end);
    L->clk += (double)(seg_end - L->cur_frame) * t;
    L->cur_frame = seg_end;
    if (fire) { if (ck == 2) apply_load(L, cand); else fire_action(L, cand); }
  }
}

/* ---------- MIDI and transport ---------- */
static double numv(const Looper* L, const LV2_Atom* a, double def) {
  if (!a) return def;
  if (a->type == L->atom_Float)  return ((const LV2_Atom_Float*)a)->body;
  if (a->type == L->atom_Double) return ((const LV2_Atom_Double*)a)->body;
  if (a->type == L->atom_Long)   return (double)((const LV2_Atom_Long*)a)->body;
  if (a->type == L->atom_Int)    return ((const LV2_Atom_Int*)a)->body;
  return def;
}

static void recalc_targets(Looper* L) {
  if (is_armed(L)) { L->arm_clk = next_boundary(L, L->arm_kind, 1); L->npre = 0; }
  if (L->pending_load) L->load_clk = next_boundary(L, L->load_kind, 1);
}

static void set_clock(Looper* L, double ht) {
  double d = ht - L->clk;
  L->clk = ht;
  if (fabs(d) > 4.0) {
    silence_loop(L);
    if (L->state == ST_REC || L->state == ST_ARM_STOP) do_clear(L);
    else recalc_targets(L);
  }
}

static void handle_pos(Looper* L, const LV2_Atom_Object* obj) {
  const LV2_Atom *bpm = NULL, *speed = NULL, *bb = NULL, *bar = NULL, *bpb = NULL;
  lv2_atom_object_get(obj, L->time_bpm, &bpm, L->time_speed, &speed, L->time_barBeat, &bb,
                      L->time_bar, &bar, L->time_bpb, &bpb, 0);
  if (bpm) { double v = numv(L, bpm, L->bpm); if (v >= 20 && v <= 400) L->bpm = v; }
  if (bpb) { int v = (int)(numv(L, bpb, 4) + 0.5); if (v >= 1 && v <= 16) { if (v != L->bpb) L->viz_dirty = 1; L->bpb = v; } }
  int has_pos = (bar && bb);
  double ht = has_pos ? (numv(L, bar, 0) * L->bpb + numv(L, bb, 0)) * PPQN : 0;
  if (speed) {
    double s = numv(L, speed, 0);
    if (s == 0) {
      if (L->rolling && !L->stop_pending) {
        L->stop_pending = 1;
        L->stop_deadline = L->samples + (uint64_t)(0.1 * L->sr);
      }
      if (has_pos) { L->stop_pos = ht; L->stop_has_pos = 1; }
      if (!L->rolling && has_pos) set_clock(L, ht);
    } else {
      L->stop_pending = 0;
      L->rolling = 1;
      if (has_pos) set_clock(L, ht);
    }
  } else if (has_pos) {
    set_clock(L, ht);
  }
}

static void handle_midi(Looper* L, const LV2_Atom_Event* ev, uint32_t f) {
  const uint8_t* b = (const uint8_t*)LV2_ATOM_BODY_CONST(&ev->body);
  uint32_t sz = ev->body.size;
  if (sz < 1) return;
  uint8_t st = b[0];
  if (st < 0x80 || st >= 0xF0 || sz > 3) {
    lv2_atom_sequence_append_event(L->out, L->out_cap, ev);
    return;
  }
  uint8_t type = st & 0xF0, ch = st & 0x0F;
  uint8_t d1 = sz > 1 ? b[1] : 0, d2 = sz > 2 ? b[2] : 0;
  int note_on = (type == 0x90 && d2 > 0);
  int note_off = (type == 0x80 || (type == 0x90 && d2 == 0));

  emit(L, f, st, d1, d2);                       /* live playing always passes through */

  if (note_on) L->held[ch][d1] = d2;
  else if (note_off) L->held[ch][d1] = 0;
  else if (type == 0xB0 && (d1 == 123 || d1 == 120)) memset(L->held[ch], 0, 128);

  if (L->state == ST_ARM_REC && advancing(L)) {
    if (note_on && (L->arm_clk - L->clk) <= PREROLL && L->npre < MAX_PRE) {
      Pre* q = &L->pre[L->npre++];
      q->ch = ch; q->p = d1; q->vel = d2; q->closed = 0; q->on_clk = L->clk; q->dur = 0;
    } else if (note_off) {
      for (int k = L->npre - 1; k >= 0; k--) {
        Pre* q = &L->pre[k];
        if (!q->closed && q->ch == ch && q->p == d1) { q->closed = 1; q->dur = L->clk - q->on_clk; break; }
      }
    }
  }

  int recording = advancing(L) &&
                  (L->state == ST_REC || L->state == ST_ARM_STOP || L->state == ST_OVERDUB);
  if (note_on) { if (recording) rec_note_on(L, ch, d1, d2); }
  else if (note_off) close_note(L, ch, d1);
  else if (type == 0xA0 || type == 0xB0 || type == 0xD0 || type == 0xE0) {
    if (type == 0xB0 && (d1 == 123 || d1 == 120)) return;
    cc_event(L, st, d1, d2);
  }
}

static void update_outputs(Looper* L) {
  double bt = bar_ticks(L), pos = 0, bars = 0;
  if (L->state == ST_REC || L->state == ST_ARM_STOP) {
    double el = (L->clk - L->rec_start) / bt;
    if (el < 0) el = 0;
    bars = el;
    if (L->auto_clk > L->rec_start) pos = (L->clk - L->rec_start) / (L->auto_clk - L->rec_start);
    else pos = el - floor(el);
  } else if (L->loop_len > 0) {
    bars = L->loop_len / bt;
    pos = pmod(L->clk - L->loop_anchor, (double)L->loop_len) / L->loop_len;
  }
  if (pos > 1) pos = 1;
  int mask = 0;
  for (int i = 0; i < L->nlayers && i < 8; i++) if (L->layers[i].muted) mask |= (1 << i);
  *L->outp[O_STATE] = (float)L->state;
  *L->outp[O_POS] = (float)pos;
  *L->outp[O_BARS] = (float)bars;
  *L->outp[O_LAYERS] = (float)L->nlayers;
  *L->outp[O_MUTE] = (float)mask;
  *L->outp[O_CONFIRM] = (float)L->confirm;
  *L->outp[O_BPM] = (float)L->bpm;
  *L->outp[O_BEAT] = (float)(pmod(L->clk, bt) / PPQN);
  *L->outp[O_RUN] = advancing(L) ? 1.0f : 0.0f;
  *L->outp[O_SLOTS] = (float)atomic_load(&L->slot_mask);
  *L->outp[O_MSG] = (float)(L->msg_seq * 100 + L->msg_code);
}

/* Safety net: a sounding loop note must always have a pending note-off; otherwise release it. */
static void reconcile_notes(Looper* L) {
  uint8_t cnt[16][128];
  memset(cnt, 0, sizeof cnt);
  for (int i = 0; i < L->noffs; i++) {
    uint8_t* q = &cnt[L->offs[i].ch & 15][L->offs[i].p & 127];
    if (*q < 255) (*q)++;
  }
  for (int c = 0; c < 16; c++)
    for (int p = 0; p < 128; p++) {
      if (L->lcnt[c][p] <= cnt[c][p]) continue;
      if (cnt[c][p] == 0) {
        if (emit(L, L->nframes ? L->nframes - 1 : 0, 0x80 | c, p, 0)) L->lcnt[c][p] = 0;
      } else {
        L->lcnt[c][p] = cnt[c][p];
      }
    }
}

/* ---------- LV2 ---------- */
static void free_all(Looper* L) {
  for (int i = 0; i < MAX_LAYERS; i++) free(L->layers[i].ev);
  snap_free(&L->bk); snap_free(&L->save_s); snap_free(&L->load_s);
  free(L->cc);
  if (L->viz) { munmap(L->viz, L->viz_size); unlink(L->viz_path); }
  if (L->viz_fd > 0) close(L->viz_fd);
  free(L);
}

static LV2_Handle instantiate(const LV2_Descriptor* d, double rate, const char* path,
                              const LV2_Feature* const* f) {
  Looper* L = (Looper*)calloc(1, sizeof(Looper));
  if (!L) return NULL;
  for (int i = 0; f[i]; i++)
    if (!strcmp(f[i]->URI, LV2_URID__map)) L->map = (LV2_URID_Map*)f[i]->data;
  if (!L->map) { free(L); return NULL; }
  int ok = 1;
  for (int i = 0; i < MAX_LAYERS; i++) {
    L->layers[i].ev = (Ev*)calloc(MAX_EVENTS, sizeof(Ev));
    if (!L->layers[i].ev) ok = 0;
  }
  if (!snap_alloc(&L->bk) || !snap_alloc(&L->save_s) || !snap_alloc(&L->load_s)) ok = 0;
  L->cc = (Ev*)calloc(MAX_CC, sizeof(Ev));
  if (!L->cc) ok = 0;
  if (!ok) { free_all(L); return NULL; }

  LV2_URID_Map* m = L->map;
  L->midi_Event    = m->map(m->handle, LV2_MIDI__MidiEvent);
  L->atom_Object   = m->map(m->handle, LV2_ATOM__Object);
  L->atom_Float    = m->map(m->handle, LV2_ATOM__Float);
  L->atom_Double   = m->map(m->handle, LV2_ATOM__Double);
  L->atom_Long     = m->map(m->handle, LV2_ATOM__Long);
  L->atom_Int      = m->map(m->handle, LV2_ATOM__Int);
  L->time_Position = m->map(m->handle, LV2_TIME__Position);
  L->time_bar      = m->map(m->handle, LV2_TIME__bar);
  L->time_barBeat  = m->map(m->handle, LV2_TIME__barBeat);
  L->time_bpm      = m->map(m->handle, LV2_TIME__beatsPerMinute);
  L->time_speed    = m->map(m->handle, LV2_TIME__speed);
  L->time_bpb      = m->map(m->handle, LV2_TIME__beatsPerBar);
  L->sr = rate; L->bpm = 120.0; L->bpb = 4;
  L->state = ST_EMPTY; L->arm_clk = -1; L->auto_clk = -1;

  /* shared memory for the widget's piano roll */
  snprintf(L->viz_path, sizeof L->viz_path, "/dev/shm/midi_looper_%d", (int)getpid());
  L->viz_size = sizeof(VizHdr) + (size_t)MAX_LAYERS * MAX_EVENTS * sizeof(Ev) + (size_t)MAX_CC * sizeof(Ev);
  L->viz_fd = open(L->viz_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (L->viz_fd > 0 && ftruncate(L->viz_fd, (off_t)L->viz_size) == 0) {
    void* mp = mmap(NULL, L->viz_size, PROT_READ | PROT_WRITE, MAP_SHARED, L->viz_fd, 0);
    if (mp != MAP_FAILED) {
      L->viz = (VizHdr*)mp;
      memset(L->viz, 0, sizeof(VizHdr));
      L->viz->magic = VIZ_MAGIC;
    }
  }

  const char* base = getenv("ZYNTHIAN_MY_DATA_DIR");
  snprintf(L->dir, sizeof L->dir, "%s/midi-looper", (base && *base) ? base : "/zynthian/zynthian-my-data");
  sem_init(&L->sem, 0, 0);
  if (pthread_create(&L->thr, NULL, worker_main, L) == 0) {
    L->thr_ok = 1;
    worker_request(L, 4, 0);
  }
  return L;
}

static void connect_port(LV2_Handle h, uint32_t port, void* data) {
  Looper* L = (Looper*)h;
  if (port == 0) L->in = (const LV2_Atom_Sequence*)data;
  else if (port == 1) L->out = (LV2_Atom_Sequence*)data;
  else if (port >= 2 && port < 2 + N_IN) L->ctl[port - 2] = (const float*)data;
  else if (port >= OUT_BASE && port < OUT_BASE + N_OUT) L->outp[port - OUT_BASE] = (float*)data;
}

static void activate(LV2_Handle h) { (void)h; }
static void deactivate(LV2_Handle h) { (void)h; }

static void run(LV2_Handle h, uint32_t n) {
  Looper* L = (Looper*)h;
  L->out_cap = L->out->atom.size;
  lv2_atom_sequence_clear(L->out);
  L->out->atom.type = L->in->atom.type;
  L->nframes = n; L->cur_frame = 0;

  poll_worker(L);
  process_controls(L);

  LV2_ATOM_SEQUENCE_FOREACH(L->in, ev) {
    uint32_t f = (uint32_t)ev->time.frames;
    if (f >= n) f = n - 1;
    if (f < L->cur_frame) f = L->cur_frame;
    advance_to(L, f);
    if (ev->body.type == L->midi_Event) handle_midi(L, ev, f);
    else if (ev->body.type == L->atom_Object) {
      const LV2_Atom_Object* o = (const LV2_Atom_Object*)&ev->body;
      if (o->body.otype == L->time_Position) handle_pos(L, o);
    }
  }
  advance_to(L, n);
  L->samples += n;

  if (L->stop_pending && L->samples >= L->stop_deadline) {
    L->stop_pending = 0; L->rolling = 0;
    if (L->stop_has_pos) { L->clk = L->stop_pos; L->stop_has_pos = 0; }
    silence_loop(L);
    if (L->state == ST_REC || L->state == ST_ARM_STOP) do_clear(L);
    else recalc_targets(L);
  }
  if (L->confirm && L->samples >= L->confirm_until) L->confirm = 0;
  reconcile_notes(L);
  update_outputs(L);
  if (L->viz_dirty && L->samples >= L->viz_next) {
    viz_export(L);
    L->viz_dirty = 0;
    L->viz_next = L->samples + (uint64_t)(0.1 * L->sr);
  }
}

static void cleanup(LV2_Handle h) {
  Looper* L = (Looper*)h;
  if (L->thr_ok) {
    atomic_store(&L->quit, 1);
    sem_post(&L->sem);
    pthread_join(L->thr, NULL);
  }
  sem_destroy(&L->sem);
  free_all(L);
}
static const void* extension_data(const char* uri) { (void)uri; return NULL; }

static const LV2_Descriptor desc = { LOOPER_URI, instantiate, connect_port, activate,
                                     run, deactivate, cleanup, extension_data };
LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t i) { return i == 0 ? &desc : NULL; }
