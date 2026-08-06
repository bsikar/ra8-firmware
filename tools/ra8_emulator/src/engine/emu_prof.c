/**
 * @file emu_prof.c
 * @brief Firmware profiler implementation (see emu_prof.h)
 *
 * @details
 * The RA8_EMU_PROFILE profiler: FUNC-symbol collection, the wall-time
 * sampler, the exact per-instruction hook with call-chain reconstruction,
 * and the run-end reports (speedscope JSON export, the self-contained HTML
 * flamechart, the boot timeline, and the inclusive/self table) -- moved
 * verbatim out of the ra8_emulator main translation unit.
 *
 * @since 0.1.0
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "emu_prof.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "emu_elf.h"

/** @brief Nanoseconds per second (timespec tv_nsec -> seconds). */
static const double s_nsec_per_sec = 1.0e9;

/** @brief Monotonic wall-clock seconds. */
double board_now_s(void)
{
  struct timespec ts = {};
  (void)clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + ((double)ts.tv_nsec / s_nsec_per_sec);
}

/* ===========================================================================
 * Firmware profiler (RA8_EMU_PROFILE). Two modes, bucketed by ELF FUNC symbol:
 *   =1     wall-time sample -- charge each chunk's wall time to its start PC.
 *          Cheap (no per-instruction cost); a flat % list of the dominant cost.
 *   =full  per-instruction -- a code hook tallies every instruction + call entry
 *          AND reconstructs the live call chain (see below), so the run end emits
 *          an Ozone-style breakdown: a boot timeline, an inclusive/self table,
 *          and a speedscope flamechart file. Accurate but ~10x slower; off by
 *          default. The run auto-stops once boot settles into the idle frame
 *          loop, so =full profiles boot work rather than the idle tail.
 * ===========================================================================
 */
enum : uint32_t {
  k_prof_max_syms = 8192U, /**< Cap on profiled FUNC symbols.      */
  k_prof_top_n    = 40U,   /**< Top entries printed in the report. */
};
typedef struct {
  uint32_t    lo;    /**< Function entry (Thumb bit cleared). */
  uint32_t    hi;    /**< Function end (lo + st_size).        */
  const char* name;  /**< Pointer into the ELF string table.  */
  double      secs;  /**< Wall seconds (wall mode).           */
  uint64_t    insns; /**< Instructions executed (insn mode).  */
  uint64_t    calls; /**< Entries to this fn (insn mode).     */
} prof_sym_t;
static prof_sym_t  s_prof[k_prof_max_syms];
static uint32_t    s_prof_n       = 0U;
static double      s_prof_total_s = 0.0;
static uint64_t    s_prof_total_i = 0U;
static prof_mode_t s_prof_mode    = k_prof_off;

/* ---------------------------------------------------------------------------
 * Ozone-style call-stack tracing (insn mode only). On top of the per-function
 * tally above, reconstruct the live call chain straight from the PC stream: a
 * fresh function entry that is not already on the chain is a call (push), and
 * re-entering a function already deeper on the chain is a return (pop down to
 * it). NASA Rule 1 bans recursion in this firmware, so a function appears at
 * most once on the chain and the "already on the chain" test is unambiguous.
 * The chain is sampled at a fixed instruction cadence into a bounded,
 * chronological store and written out as a speedscope "sampled" profile -- open
 * ra8_emulator_profile.speedscope.json at https://speedscope.app for the
 * time-ordered flamechart ("what ran when", the Ozone timeline) plus the
 * sandwich view (self vs total per function). WFI idle naturally weighs ~zero
 * because a halted core retires no instructions, so the picture is boot work,
 * not the idle frame loop. ===============================================
 */
enum : uint32_t {
  k_prof_max_depth   = 64U,    /**< Deepest call chain captured per sample.  */
  k_prof_max_samples = 16384U, /**< Chronological stack samples (decimated). */
  k_prof_samp_every  = 256U,   /**< Default instructions per chain sample.   */
};
static uint16_t s_pstk[k_prof_max_depth];                      /**< Live chain.                */
static uint32_t s_pstk_n = 0U;                                 /**< Chain depth.               */
static uint16_t s_samp[k_prof_max_samples][k_prof_max_depth];  /**< root..leaf.                */
static uint8_t  s_samp_d[k_prof_max_samples];                  /**< Per-sample chain depth.    */
static uint32_t s_samp_w[k_prof_max_samples];                  /**< Per-sample weight (insns). */
static uint32_t s_samp_n        = 0U;                          /**< Stored sample count.       */
static uint64_t s_samp_every    = (uint64_t)k_prof_samp_every; /**< Insns per sample (>>x2).   */
static uint64_t s_samp_acc      = 0U;                          /**< Insns since last sample.   */
static uint32_t s_prof_stop_pc  = 0U;                          /**< RA8_EMU_STOP_PC (0=off).   */
static bool     s_prof_stop_hit = false;                       /**< Set when STOP_PC reached.  */
static uint64_t s_incl[k_prof_max_syms];                       /**< Inclusive weight (report). */
static uint64_t s_self[k_prof_max_syms];                       /**< Self (leaf) weight.        */

/** @brief qsort comparator: order the FUNC symbols by entry address. */
static int prof_cmp(const void* a, const void* b)
{
  const uint32_t la = ((const prof_sym_t*)a)->lo;
  const uint32_t lb = ((const prof_sym_t*)b)->lo;
  if (la < lb) {
    return -1;
  }
  if (la > lb) {
    return 1;
  }
  return 0;
}

/** @brief Collect the STT_FUNC symbols of one SHT_SYMTAB section into s_prof. */
static void prof_collect_symtab(const uint8_t* elf,
                                const uint8_t* sh,
                                uint32_t       shoff,
                                uint16_t       shentsize,
                                uint16_t       shnum)
{
  uint32_t sym_off     = 0U;
  uint32_t sym_size    = 0U;
  uint32_t sym_link    = 0U;
  uint32_t sym_entsize = 0U;
  (void)memcpy(&sym_off, sh + 16, 4);
  (void)memcpy(&sym_size, sh + (uint32_t)k_elf_sh_size_off, 4);
  (void)memcpy(&sym_link, sh + (uint32_t)k_elf_sh_link_off, 4);
  (void)memcpy(&sym_entsize, sh + (uint32_t)k_elf_sh_entsize_off, 4);
  if ((sym_entsize < 16U) || (sym_link >= shnum)) {
    return;
  }
  const uint8_t* strsh   = elf + shoff + ((size_t)(uint32_t)sym_link * shentsize);
  uint32_t       str_off = 0U;
  (void)memcpy(&str_off, strsh + 16, 4);
  const uint32_t nsym = sym_size / sym_entsize;
  for (uint32_t s = 0U; (s < nsym) && (s_prof_n < (uint32_t)k_prof_max_syms); s++) {
    const uint8_t* sym      = elf + sym_off + ((size_t)s * sym_entsize);
    uint32_t       st_name  = 0U;
    uint32_t       st_value = 0U;
    uint32_t       st_size  = 0U;
    (void)memcpy(&st_name, sym + 0, 4);
    (void)memcpy(&st_value, sym + 4, 4);
    (void)memcpy(&st_size, sym + 8, 4);
    if (((sym[k_elf_sym_info_off] & (uint8_t)k_elf_st_type_mask) != 2U) || (st_size == 0U) ||
        (st_name == 0U)) { /* STT_FUNC */
      continue;
    }
    s_prof[s_prof_n].lo    = st_value & ~1U;
    s_prof[s_prof_n].hi    = (st_value & ~1U) + st_size;
    s_prof[s_prof_n].name  = (const char*)(elf + str_off + st_name);
    s_prof[s_prof_n].secs  = 0.0;
    s_prof[s_prof_n].insns = 0U;
    s_prof[s_prof_n].calls = 0U;
    s_prof_n++;
  }
}

/** @brief Collect + sort FUNC symbols (RA8_EMU_PROFILE only) for PC bucketing. */
void prof_load(const uint8_t* elf, long len)
{
  const char* mode = getenv("RA8_EMU_PROFILE");
  if (mode == nullptr) {
    s_prof_mode = k_prof_off;
    return;
  }
  s_prof_mode =
    ((strcmp(mode, "full") == 0) || (strcmp(mode, "insn") == 0)) ? k_prof_insn : k_prof_wall;
  if (len < (long)k_elf_ehdr_size) {
    return;
  }
  uint32_t shoff = 0U;
  (void)memcpy(&shoff, elf + 32, 4);
  const uint16_t shentsize = (uint16_t)(elf[46] | (elf[47] << 8));
  const uint16_t shnum     = (uint16_t)(elf[48] | (elf[49] << 8));
  if (shoff == 0U) {
    return;
  }
  for (uint16_t i = 0U; i < shnum; i++) {
    const uint8_t* sh      = elf + shoff + ((size_t)(uint32_t)i * shentsize);
    uint32_t       sh_type = 0U;
    (void)memcpy(&sh_type, sh + 4, 4);
    if (sh_type != 2U) { /* SHT_SYMTAB */
      continue;
    }
    prof_collect_symtab(elf, sh, shoff, shentsize, shnum);
  }
  qsort(s_prof, (size_t)s_prof_n, sizeof(s_prof[0]), prof_cmp);
  (void)fprintf(stderr,
                "  [profile] %s; %u FUNC symbols\n",
                (s_prof_mode == k_prof_insn) ? "per-instruction (exact, slow)" : "wall-time sample",
                (unsigned)s_prof_n);
}

/** @brief Binary-search the FUNC symbol owning @p pc; returns s_prof_n if none. */
static uint32_t prof_find(uint32_t pc)
{
  uint32_t lo = 0U;
  uint32_t hi = s_prof_n;
  while (lo < hi) {
    const uint32_t mid = lo + ((hi - lo) / 2U);
    if (s_prof[mid].lo <= pc) {
      lo = mid + 1U;
    } else {
      hi = mid;
    }
  }
  if (lo == 0U) {
    return s_prof_n;
  }
  const uint32_t idx = lo - 1U;
  return (pc < s_prof[idx].hi) ? idx : s_prof_n;
}

/** @brief Attribute @p dt wall seconds to @p pc's function (wall-sample mode). */
void prof_add(uint32_t pc, double dt)
{
  if (s_prof_mode != k_prof_wall) {
    return;
  }
  s_prof_total_s += dt;
  const uint32_t idx = prof_find(pc);
  if (idx < s_prof_n) {
    s_prof[idx].secs += dt;
  }
}

/** @brief Halve the sample store (merge adjacent pairs) when it fills up. */
static void prof_decimate(void)
{
  uint32_t dst = 0U;
  for (uint32_t i = 0U; i < s_samp_n; i += 2U) {
    const uint32_t w2 = ((i + 1U) < s_samp_n) ? s_samp_w[i + 1U] : 0U;
    if (dst != i) {
      (void)memcpy(s_samp[dst], s_samp[i], (size_t)s_samp_d[i] * sizeof(uint16_t));
      s_samp_d[dst] = s_samp_d[i];
    }
    s_samp_w[dst] = s_samp_w[i] + w2; /* merged time keeps the total exact. */
    dst++;
  }
  s_samp_n = dst;
  s_samp_every *= 2U; /* coarser cadence keeps the next fill the same span. */
}

/** @brief Append the live call chain as one chronological sample of @p weight insns. */
static void prof_sample(uint32_t weight)
{
  if (s_samp_n >= (uint32_t)k_prof_max_samples) {
    prof_decimate();
  }
  uint32_t d = s_pstk_n;
  if (d > (uint32_t)k_prof_max_depth) {
    d = (uint32_t)k_prof_max_depth;
  }
  for (uint32_t i = 0U; i < d; i++) {
    s_samp[s_samp_n][i] = s_pstk[i];
  }
  s_samp_d[s_samp_n] = (uint8_t)d;
  s_samp_w[s_samp_n] = weight;
  s_samp_n++;
}

/** @brief Fold @p f (PC's owning FUNC index) into the live call chain (push/pop). */
static void prof_stack_update(uint32_t f)
{
  if (f >= s_prof_n) {
    return; /* unknown region -- keep the current leaf (it gets the self time). */
  }
  if ((s_pstk_n > 0U) && (s_pstk[s_pstk_n - 1U] == (uint16_t)f)) {
    return; /* still in the same function -- no call/return transition. */
  }
  for (uint32_t i = s_pstk_n; i > 0U; i--) {
    if (s_pstk[i - 1U] == (uint16_t)f) {
      s_pstk_n = i; /* returned to a frame already on the chain -- unwind to it. */
      return;
    }
  }
  if (s_pstk_n < (uint32_t)k_prof_max_depth) {
    s_pstk[s_pstk_n] = (uint16_t)f; /* a fresh call -- push it. */
    s_pstk_n++;
  }
}

/** @brief UC_HOOK_CODE per instruction: tally instructions + calls + call chain. */
/* cppcheck-suppress constParameterCallback ; UC_HOOK_CODE callback ABI is void*. */
static void prof_insn_hook(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)size;
  (void)user;
  s_prof_total_i++;
  const uint32_t idx = prof_find((uint32_t)address);
  if (idx < s_prof_n) {
    s_prof[idx].insns++;
    if ((uint32_t)address == s_prof[idx].lo) {
      s_prof[idx].calls++; /* PC at the entry point -> a fresh call (approx). */
    }
  }
  prof_stack_update(idx);
  s_samp_acc++;
  if (s_samp_acc >= s_samp_every) {
    prof_sample((uint32_t)s_samp_acc);
    s_samp_acc = 0U;
  }
  if ((s_prof_stop_pc != 0U) && ((uint32_t)address == s_prof_stop_pc)) {
    s_prof_stop_hit = true; /* RA8_EMU_STOP_PC reached -- end the run cleanly. */
    (void)uc_emu_stop(uc);
  }
}

/* @brief Write the captured call chains as a speedscope "sampled" profile JSON. */
/**
 * @brief Write the speedscope `shared.frames` array: one entry per symbol.
 *
 * @details
 * Frame names come from the ELF symbol table and are emitted inside JSON
 * strings, so `"` and `\` are escaped as they are copied.
 *
 * @param[in,out] f Open output stream positioned after the `"frames":[`.
 *
 * @pre @p f is non-NULL and writable.
 * @pre `s_prof_n` frames are populated.
 * @post Exactly `s_prof_n` comma-separated objects are written.
 * @post The enclosing bracket is left for the caller to close.
 *
 * @note Not thread-safe; the profiler is single-threaded host-side.
 */
static void prof_json_frames(FILE* f)
{
  for (uint32_t i = 0U; i < s_prof_n; i++) {
    (void)fputs((i == 0U) ? "{\"name\":\"" : ",{\"name\":\"", f);
    for (const char* p = s_prof[i].name; *p != '\0'; p++) { /* JSON-escape. */
      if ((*p == '"') || (*p == '\\')) {
        (void)fputc('\\', f);
      }
      (void)fputc(*p, f);
    }
    (void)fputs("\"}", f);
  }
}

/**
 * @brief Write the speedscope `samples` array: one frame-index stack per sample.
 *
 * @param[in,out] f Open output stream positioned after the `"samples":[`.
 *
 * @pre @p f is non-NULL and writable.
 * @pre `s_samp_n` samples are populated with matching depths in `s_samp_d`.
 * @post Exactly `s_samp_n` comma-separated arrays are written.
 * @post The enclosing bracket is left for the caller to close.
 *
 * @note Not thread-safe; the profiler is single-threaded host-side.
 */
static void prof_json_samples(FILE* f)
{
  for (uint32_t i = 0U; i < s_samp_n; i++) {
    (void)fputc((i == 0U) ? '[' : ',', f);
    if (i != 0U) {
      (void)fputc('[', f);
    }
    for (uint8_t j = 0U; j < s_samp_d[i]; j++) {
      (void)fprintf(f, (j == 0U) ? "%u" : ",%u", (unsigned)s_samp[i][j]);
    }
    (void)fputc(']', f);
  }
}

/**
 * @brief Write the speedscope `weights` array, parallel to the samples array.
 *
 * @param[in,out] f Open output stream positioned after the `"weights":[`.
 *
 * @pre @p f is non-NULL and writable.
 * @pre `s_samp_w` holds `s_samp_n` weights.
 * @post Exactly `s_samp_n` comma-separated integers are written.
 * @post The enclosing bracket is left for the caller to close.
 *
 * @note Not thread-safe; the profiler is single-threaded host-side.
 */
static void prof_json_weights(FILE* f)
{
  for (uint32_t i = 0U; i < s_samp_n; i++) {
    (void)fprintf(f, (i == 0U) ? "%u" : ",%u", (unsigned)s_samp_w[i]);
  }
}

/**
 * @brief Write the flamechart page's `FRAMES` array as JavaScript string literals.
 *
 * @details
 * The same frame names as @ref prof_json_frames, but the viewer markup quotes
 * with `'`, so `'` and `\` are the characters escaped here.
 *
 * @param[in,out] f Open output stream positioned after the `FRAMES=[`.
 *
 * @pre @p f is non-NULL and writable.
 * @pre `s_prof_n` frames are populated.
 * @post Exactly `s_prof_n` comma-separated quoted names are written.
 * @post The enclosing bracket is left for the caller to close.
 *
 * @note Not thread-safe; the profiler is single-threaded host-side.
 */
static void prof_js_frames(FILE* f)
{
  for (uint32_t i = 0U; i < s_prof_n; i++) {
    (void)fputs((i == 0U) ? "'" : ",'", f);
    for (const char* p = s_prof[i].name; *p != '\0'; p++) { /* escape ' and backslash for JS. */
      if ((*p == '\'') || (*p == '\\')) {
        (void)fputc('\\', f);
      }
      (void)fputc(*p, f);
    }
    (void)fputc('\'', f);
  }
}

static void prof_write_speedscope(const char* path)
{
  if ((s_samp_n == 0U) || (s_prof_n == 0U)) {
    return;
  }
  FILE* f = fopen(path, "w");
  if (f == NULL) {
    return;
  }
  uint64_t total = 0U;
  for (uint32_t i = 0U; i < s_samp_n; i++) {
    total += s_samp_w[i];
  }
  (void)fprintf(f,
                "{\"$schema\":\"https://www.speedscope.app/file-format-schema.json\",\n"
                " \"name\":\"ra8_emulator boot\",\"activeProfileIndex\":0,\n"
                " \"shared\":{\"frames\":[");
  prof_json_frames(f);
  (void)fprintf(f,
                "]},\n"
                " \"profiles\":[{\"type\":\"sampled\",\"name\":\"boot\",\"unit\":\"none\",\n"
                "  \"startValue\":0,\"endValue\":%llu,\n"
                "  \"samples\":[",
                (unsigned long long)total);
  prof_json_samples(f);
  (void)fputs("],\n  \"weights\":[", f);
  prof_json_weights(f);
  (void)fputs("]}]}\n", f);
  (void)fclose(f);
}

/* Self-contained flamechart viewer markup. The page embeds the profile arrays
 * (written just before this) and renders a time-ordered flame chart on a canvas
 * -- the Ozone timeline, but as a local file that opens in any browser with no
 * upload and no external site. Single-quoted HTML/JS strings keep the C literal
 * free of escapes; pure 7-bit ASCII. */
static const char k_prof_html_head[] =
  "<!doctype html><html><head><meta charset='utf-8'><title>ra8_emulator profile</title>\n"
  "<style>\n"
  "body{margin:0;font:12px Menlo,monospace;background:#1e1e1e;color:#ddd}\n"
  "#bar{padding:7px 10px;background:#2a2a2a;border-bottom:1px solid #444}\n"
  "#bar b{color:#fff}#bar button,#bar input{font:11px monospace;margin-left:10px;"
  "background:#3a3a3a;color:#ddd;border:1px solid #555;padding:2px 6px}\n"
  "#tip{position:fixed;pointer-events:none;background:#000;color:#fff;padding:5px 7px;"
  "border:1px solid #888;display:none;white-space:nowrap;z-index:9;font:11px monospace}\n"
  "canvas{display:block;cursor:crosshair}\n"
  "</style></head><body>\n"
  "<div id='bar'><b id='title'></b><span id='info'></span>"
  "<button onclick='resetView()'>Reset zoom</button>"
  "search:<input id='q' size='18' oninput='onSearch()'></div>\n"
  "<canvas id='fc'></canvas><div id='tip'></div>\n<script>\n";

static const char k_prof_html_js[] =
  "var cv=document.getElementById('fc'),ctx=cv.getContext('2d'),tip=document.getElementById('tip');\n"
  "document.getElementById('title').textContent=TITLE;\n"
  "var ROW=18,total=0,i;for(i=0;i<WEIGHTS.length;i++)total+=WEIGHTS[i];\n"
  "var maxd=0;for(i=0;i<SAMPLES.length;i++)if(SAMPLES[i].length>maxd)maxd=SAMPLES[i].length;\n"
  "var rects=[],incl={},self={};\n"
  "for(var d=0;d<maxd;d++){var cum=0,rs=0,rf=-1,op=false;\n"
  " for(i=0;i<SAMPLES.length;i++){var ff=d<SAMPLES[i].length?SAMPLES[i][d]:-1;\n"
  "  if(!(op&&ff===rf)){if(op&&rf>=0)rects.push({d:d,a:rs,b:cum,f:rf});rs=cum;rf=ff;op=true;}\n"
  "  cum+=WEIGHTS[i];}\n"
  " if(op&&rf>=0)rects.push({d:d,a:rs,b:cum,f:rf});}\n"
  "for(i=0;i<SAMPLES.length;i++){var s=SAMPLES[i],w=WEIGHTS[i];\n"
  " for(var j=0;j<s.length;j++)incl[s[j]]=(incl[s[j]]||0)+w;\n"
  " if(s.length)self[s[s.length-1]]=(self[s[s.length-1]]||0)+w;}\n"
  "var vx0=0,vx1=total,q='';\n"
  "function col(fi){var n=FRAMES[fi],h=0,k;for(k=0;k<n.length;k++)h=(h*31+n.charCodeAt(k))&0xffffff;\n"
  " var lit=(q&&n.toLowerCase().indexOf(q)>=0);return 'hsl('+(h%359)+','+(lit?'90%':'48%')+','+(lit?'62%':'44%')+')';}\n"
  "function resize(){cv.width=window.innerWidth;cv.height=Math.max(maxd*ROW+4,160);draw();}\n"
  "function draw(){ctx.clearRect(0,0,cv.width,cv.height);var span=vx1-vx0;if(span<=0)return;\n"
  " ctx.font='11px monospace';ctx.textBaseline='middle';\n"
  " for(var r=0;r<rects.length;r++){var R=rects[r];if(R.b<=vx0||R.a>=vx1)continue;\n"
  "  var p0=(R.a-vx0)/span*cv.width,p1=(R.b-vx0)/span*cv.width,w=p1-p0;if(w<0.4)continue;\n"
  "  var y=R.d*ROW;ctx.fillStyle=col(R.f);ctx.fillRect(p0,y,Math.max(w-0.6,0.5),ROW-1);\n"
  "  if(w>34){ctx.fillStyle='#111';ctx.fillText(FRAMES[R.f],p0+3,y+ROW/2,w-6);}}}\n"
  "function pick(mx,my){var d=Math.floor(my/ROW),span=vx1-vx0,wx=vx0+mx/cv.width*span,r;\n"
  " for(r=0;r<rects.length;r++){var R=rects[r];if(R.d===d&&wx>=R.a&&wx<R.b)return R;}return null;}\n"
  "cv.onmousemove=function(e){var R=pick(e.offsetX,e.offsetY);if(!R){tip.style.display='none';return;}\n"
  " var n=FRAMES[R.f],to=incl[R.f]||0,se=self[R.f]||0;\n"
  " tip.innerHTML=n+'<br>this block: '+((R.b-R.a)/total*100).toFixed(2)+'% ('+(R.b-R.a)+' insns)'+\n"
  "  '<br>total '+(to/total*100).toFixed(2)+'%  self '+(se/total*100).toFixed(2)+'%';\n"
  " tip.style.display='block';tip.style.left=(e.clientX+14)+'px';tip.style.top=(e.clientY+14)+'px';};\n"
  "cv.onmouseleave=function(){tip.style.display='none';};\n"
  "cv.onclick=function(e){var R=pick(e.offsetX,e.offsetY);if(R){vx0=R.a;vx1=R.b;draw();}};\n"
  "function resetView(){vx0=0;vx1=total;draw();}\n"
  "function onSearch(){q=document.getElementById('q').value.toLowerCase();draw();}\n"
  "document.getElementById('info').textContent=' | '+SAMPLES.length+' samples, '+total+\n"
  "  ' insns  (hover for self/total, click a block to zoom, Reset to zoom out)';\n"
  "window.onresize=resize;resize();\n";

/** @brief Write a self-contained, locally-openable HTML flamechart of the samples. */
static void prof_write_html(const char* path, uint64_t total)
{
  if ((s_samp_n == 0U) || (s_prof_n == 0U)) {
    return;
  }
  FILE* f = fopen(path, "w");
  if (f == NULL) {
    return;
  }
  (void)fputs(k_prof_html_head, f);
  (void)fputs("var FRAMES=[", f);
  prof_js_frames(f);
  (void)fputs("];\n", f);
  /* SAMPLES / WEIGHTS are plain integer arrays, so the JSON the speedscope
   * writer emits is already valid JavaScript -- the same two helpers serve. */
  (void)fputs("var SAMPLES=[", f);
  prof_json_samples(f);
  (void)fputs("];\n", f);
  (void)fputs("var WEIGHTS=[", f);
  prof_json_weights(f);
  (void)fputs("];\n", f);
  (void)fprintf(f,
                "var TITLE='ra8_emulator flamechart -- %llu insns, %u samples';\n",
                (unsigned long long)total,
                (unsigned)s_samp_n);
  (void)fputs(k_prof_html_js, f);
  (void)fputs("</script></body></html>\n", f);
  (void)fclose(f);
}

/** @brief Fraction-to-per-cent scale (fraction * 100.0 == per-cent). */
static const double s_percent_scale = 100.0;

/** @brief Boot-timeline "phase" collapse constants for prof_print_boot_timeline. */
typedef enum : uint32_t {
  k_no_fn        = 0xFFFFFFFFU, /**< Sentinel for "no phase frame".        */
  k_phase_depth  = 2U,          /**< Chain depth used as the boot "phase". */
  k_phase_lines  = 24U,         /**< Cap on printed timeline segments.     */
  k_phase_pct_x1 = 100U,        /**< Per-cent base: keep segments >= 1%.   */
} prof_phase_t;

/** @brief Reset then fill s_incl/s_self from the samples; return total weight. */
static uint64_t prof_accumulate_incl_self(void)
{
  /* Inclusive (anywhere on the chain) + self (leaf) weights, from the samples.
   * No recursion (NASA Rule 1) -> each function appears at most once per sample,
   * so a straight per-frame add needs no dedup. */
  uint64_t total = 0U;
  for (uint32_t i = 0U; i < s_prof_n; i++) {
    s_incl[i] = 0U;
    s_self[i] = 0U;
  }
  for (uint32_t i = 0U; i < s_samp_n; i++) {
    const uint32_t w = s_samp_w[i];
    const uint8_t  d = s_samp_d[i];
    total += w;
    for (uint8_t j = 0U; j < d; j++) {
      s_incl[s_samp[i][j]] += w;
    }
    if (d > 0U) {
      s_self[s_samp[i][d - 1U]] += w;
    }
  }
  return total;
}

/** @brief Print the boot timeline: each contiguous shallow-depth phase run. */
static void prof_print_boot_timeline(uint64_t total)
{
  /* Phase timeline: collapse each sample's chain to a fixed shallow depth (the
   * major subsystem under main) and print each contiguous run as a boot phase,
   * so the terminal shows "what ran when" even without opening speedscope. */
  (void)fprintf(stderr,
                "  [profile] boot timeline (phase = call depth %u; start%% .. width%%):\n",
                (unsigned)k_phase_depth);
  uint64_t cum   = 0U;
  uint64_t segw  = 0U;
  uint32_t segfn = (uint32_t)k_no_fn;
  uint32_t lines = 0U;
  for (uint32_t i = 0U; i <= s_samp_n; i++) {
    uint32_t fn = (uint32_t)k_no_fn;
    if (i < s_samp_n) {
      const uint8_t d = s_samp_d[i];
      if (d > 0U) {
        const uint8_t pd = ((uint32_t)(d - 1U) < (uint32_t)k_phase_depth) ? (uint8_t)(d - 1U)
                                                                          : (uint8_t)k_phase_depth;
        fn               = s_samp[i][pd];
      }
    }
    if ((i == s_samp_n) || (fn != segfn)) {
      const bool show = (segfn != (uint32_t)k_no_fn) &&
                        (((uint32_t)k_phase_pct_x1 * segw) >= total) &&
                        (lines < (uint32_t)k_phase_lines);
      if (show) {
        (void)fprintf(stderr,
                      "    %5.1f%%  +%4.1f%%  %s\n",
                      s_percent_scale * (double)cum / (double)total,
                      s_percent_scale * (double)segw / (double)total,
                      (segfn < s_prof_n) ? s_prof[segfn].name : "?");
        lines++;
      }
      cum += segw;
      segw  = 0U;
      segfn = fn;
    }
    if (i < s_samp_n) {
      segw += s_samp_w[i];
    }
  }
}

/** @brief Print the inclusive/self table (sorted by inclusive weight). */
static void prof_print_incl_self_table(const char* html, const char* out, uint64_t total)
{
  /* Inclusive/self table -- the "why is it slow" view (sorted by inclusive). */
  (void)fprintf(stderr,
                "  [profile] flamechart GUI -> %s  (interactive: hover/zoom/search)\n"
                "  [profile] %u samples over %llu insns  (also %s for speedscope.app)\n"
                "       self%%    total%%  function\n",
                html,
                (unsigned)s_samp_n,
                (unsigned long long)total,
                out);
  for (uint32_t k = 0U; k < (uint32_t)k_prof_top_n; k++) {
    uint32_t best  = s_prof_n;
    uint64_t bestv = 0U;
    for (uint32_t i = 0U; i < s_prof_n; i++) {
      if (s_incl[i] > bestv) {
        bestv = s_incl[i];
        best  = i;
      }
    }
    if ((best == s_prof_n) || (bestv == 0U)) {
      break;
    }
    (void)fprintf(stderr,
                  "    %8.2f%%  %7.2f%%  %s\n",
                  s_percent_scale * (double)s_self[best] / (double)total,
                  s_percent_scale * (double)s_incl[best] / (double)total,
                  s_prof[best].name);
    s_incl[best] = 0U; /* consume so the next pick is the runner-up. */
  }
}

/** @brief Speedscope export + inclusive/self breakdown + phase timeline (insn mode). */
static void prof_report_flamechart(void)
{
  const char* out = getenv("RA8_EMU_PROFILE_OUT");
  if ((out == nullptr) || (out[0] == '\0')) {
    out = "ra8_emulator_profile.speedscope.json";
  }
  const char* html = getenv("RA8_EMU_PROFILE_HTML");
  if ((html == nullptr) || (html[0] == '\0')) {
    html = "ra8_emulator_profile.html";
  }
  prof_write_speedscope(out);

  const uint64_t total = prof_accumulate_incl_self();
  if (total == 0U) {
    return;
  }
  prof_write_html(html, total); /* self-contained local GUI flamechart. */
  prof_print_boot_timeline(total);
  prof_print_incl_self_table(html, out, total);
}

/** @brief Print the top hot functions (by wall time or instruction count) at run end. */
void prof_report(void)
{
  const bool   insn = (s_prof_mode == k_prof_insn);
  const double tot  = insn ? (double)s_prof_total_i : s_prof_total_s;
  if ((s_prof_mode == k_prof_off) || (tot <= 0.0)) {
    return;
  }
  if (insn) {
    (void)fprintf(stderr,
                  "  [profile] %llu instructions; hottest (by instruction count):\n"
                  "     %%insn       instructions       calls  function\n",
                  (unsigned long long)s_prof_total_i);
  } else {
    (void)fprintf(stderr, "  [profile] %.2fs wall sampled; hottest (by wall time):\n", tot);
  }
  for (uint32_t k = 0U; k < (uint32_t)k_prof_top_n; k++) {
    uint32_t best  = s_prof_n;
    double   bestv = 0.0;
    for (uint32_t i = 0U; i < s_prof_n; i++) {
      const double v = insn ? (double)s_prof[i].insns : s_prof[i].secs;
      if (v > bestv) {
        bestv = v;
        best  = i;
      }
    }
    if ((best == s_prof_n) || (bestv <= 0.0)) {
      break;
    }
    if (insn) {
      (void)fprintf(stderr,
                    "    %6.2f%%  %15llu  %10llu  %s\n",
                    s_percent_scale * bestv / tot,
                    (unsigned long long)s_prof[best].insns,
                    (unsigned long long)s_prof[best].calls,
                    s_prof[best].name);
      s_prof[best].insns = 0U;
    } else {
      (void)fprintf(stderr,
                    "    %6.2f%%  %8.2fs  %s\n",
                    s_percent_scale * bestv / tot,
                    bestv,
                    s_prof[best].name);
      s_prof[best].secs = 0.0;
    }
  }
  if (insn && (s_samp_n > 0U)) {
    prof_report_flamechart(); /* speedscope export + inclusive/self + timeline. */
  }
}

/** @brief Implementation of `emu_prof_install()` -- arms the insn hook in insn mode. */
void emu_prof_install(uc_engine* uc)
{
  if (s_prof_mode == k_prof_insn) {
    static uc_hook s_h_prof;
    (void)uc_hook_add(uc, &s_h_prof, UC_HOOK_CODE, (void*)prof_insn_hook, nullptr, 1, 0);
  }
}

/** @brief Implementation of `emu_prof_mode()` -- plain state read. */
prof_mode_t emu_prof_mode(void)
{
  return s_prof_mode;
}

/** @brief Implementation of `emu_prof_total_insns()` -- plain counter read. */
uint64_t emu_prof_total_insns(void)
{
  return s_prof_total_i;
}

/** @brief Implementation of `emu_prof_set_stop_pc()` -- plain state write. */
void emu_prof_set_stop_pc(uint32_t pc)
{
  s_prof_stop_pc = pc;
}

/** @brief Implementation of `emu_prof_stop_hit()` -- plain flag read. */
bool emu_prof_stop_hit(void)
{
  return s_prof_stop_hit;
}
