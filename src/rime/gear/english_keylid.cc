//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 蓬萊：中英混輸的按鍵層語言辨識（translator/english_keylid）
//
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <unordered_map>
#include <rime/gear/english_keylid.h>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace rime {

namespace {

const char kMagic[8] = {'P', 'L', 'K', 'E', 'Y', 'L', 'D', '1'};
const size_t kHeader = 8 + 64 + 48 * 2 + 16;
const int kOrder = 5;
const uint32_t kMask = (1u << 24) - 1;  // 前文：最後 4 個符號
const double kNegInf = -std::numeric_limits<double>::infinity();

inline size_t align8(size_t x) {
  return (x + 7) & ~static_cast<size_t>(7);
}

inline bool is_letter(char c) {
  return c >= 'a' && c <= 'z';
}

}  // namespace

std::shared_ptr<EnglishKeylid> EnglishKeylid::Open(const std::string& file,
                                                   std::string* error) {
  std::shared_ptr<EnglishKeylid> model(new EnglishKeylid);
#ifdef _WIN32
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    *error = "cannot open";
    return nullptr;
  }
  model->owned_.assign(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
  model->base_ = &model->owned_[0];
  model->size_ = model->owned_.size();
#else
  int fd = ::open(file.c_str(), O_RDONLY);
  if (fd < 0) {
    *error = "cannot open";
    return nullptr;
  }
  struct stat st;
  if (::fstat(fd, &st) != 0 || static_cast<size_t>(st.st_size) < kHeader) {
    ::close(fd);
    *error = "too small";
    return nullptr;
  }
  void* base = ::mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  ::close(fd);
  if (base == MAP_FAILED) {
    *error = "mmap failed";
    return nullptr;
  }
  model->base_ = base;
  model->size_ = st.st_size;
  model->mapped_ = true;
#endif
  const char* p = static_cast<const char*>(model->base_);
  if (model->size_ < kHeader || std::memcmp(p, kMagic, 8) != 0) {
    *error = "not a keylid table (magic)";
    return nullptr;
  }
  const unsigned char* sym = reinterpret_cast<const unsigned char*>(p + 8);
  bool used[64] = {};
  for (int i = 1; i < 64; ++i) {
    if (sym[i]) {
      model->id_of_[sym[i]] = static_cast<uint8_t>(i);
      used[i] = true;
    }
  }
  model->begin_ = model->id_of_[static_cast<unsigned char>('^')];
  model->end_ = model->id_of_[static_cast<unsigned char>('$')];
  model->unknown_ = 0;
  for (int i = 63; i > 0 && !model->unknown_; --i) {
    if (!used[i])
      model->unknown_ = static_cast<uint8_t>(i);
  }
  if (!model->begin_ || !model->end_ || !model->unknown_) {
    *error = "bad symbol table";
    return nullptr;
  }
  auto section = [&](const char* at, const uint32_t** keys,
                     const float** values, size_t* count) {
    uint64_t off, n;
    std::memcpy(&off, at, 8);
    std::memcpy(&n, at + 8, 8);
    if (off % 8 != 0 || off > model->size_ || n > (model->size_ - off) / 4)
      return false;
    size_t voff = align8(off + n * 4);
    if (voff > model->size_ || n > (model->size_ - voff) / 4)
      return false;
    *keys = reinterpret_cast<const uint32_t*>(p + off);
    *values = reinterpret_cast<const float*>(p + voff);
    *count = n;
    return true;
  };
  Model* models[2] = {&model->zh_, &model->en_};
  for (int k = 0; k < 2; ++k) {
    const char* at = p + 72 + 48 * k;
    Model* m = models[k];
    if (!section(at, &m->gram_key, &m->gram_logp, &m->grams) ||
        !section(at + 16, &m->ctx_key, &m->ctx_logbo, &m->ctxs)) {
      *error = "bad section " + std::to_string(k);
      return nullptr;
    }
    std::memcpy(&m->log_v, at + 32, 4);
    std::memcpy(&m->bo_empty, at + 36, 4);
    if (m->grams == 0) {
      *error = "empty model " + std::to_string(k);
      return nullptr;
    }
  }
  return model;
}

EnglishKeylid::~EnglishKeylid() {
#ifndef _WIN32
  if (mapped_ && base_)
    ::munmap(base_, size_);
#endif
}

// 同 tools/gen_keylid.py 的 Table.lp（不剪枝時跟 keylid.py 的 NG.lp 相同）
double EnglishKeylid::Lp(const Model& m, uint32_t h, uint32_t x) const {
  double acc = 0;
  for (int k = kOrder - 1; k >= 0; --k) {
    const uint32_t hh = k == 0 ? 0 : (h & ((1u << (6 * k)) - 1));
    const uint32_t g = (hh << 6) | x;
    const uint32_t* q = std::lower_bound(m.gram_key, m.gram_key + m.grams, g);
    if (q != m.gram_key + m.grams && *q == g)
      return acc + m.gram_logp[q - m.gram_key];
    if (k == 0) {
      acc += m.bo_empty;
    } else {
      const uint32_t* c = std::lower_bound(m.ctx_key, m.ctx_key + m.ctxs, hh);
      if (c != m.ctx_key + m.ctxs && *c == hh)
        acc += m.ctx_logbo[c - m.ctx_key];
    }
  }
  return acc + m.log_v;
}

std::unique_ptr<EnglishKeylid::Lattice> EnglishKeylid::Analyze(
    const std::string& input,
    const Params& params) const {
  std::unique_ptr<Lattice> lat(new Lattice);
  lat->model_ = this;
  lat->params_ = params;
  lat->input_ = input;
  const size_t n = input.size();
  const size_t lmin = static_cast<size_t>((std::max)(1, params.min_length));
  const size_t lmax = (std::max)(lmin, static_cast<size_t>((std::max)(
                                           1, params.max_length)));
  const size_t width = lmax + 1;
  lat->width_ = width;
  const double switch_cost = -params.switch_penalty;
  const size_t beam = static_cast<size_t>((std::max)(1, params.beam));

  lat->ids_.resize(n);
  for (size_t i = 0; i < n; ++i) {
    uint8_t id = id_of_[static_cast<unsigned char>(input[i])];
    lat->ids_[i] = id ? id : unknown_;
  }
  lat->run_end_.assign(n, 0);
  for (size_t i = n; i-- > 0;) {
    if (is_letter(input[i]))
      lat->run_end_[i] =
          (i + 1 < n && is_letter(input[i + 1])) ? lat->run_end_[i + 1] : i + 1;
  }
  // 每一段英文的字母分數（從 '^' 開始、含結尾 '$'）
  lat->en_.assign(n * width, kNegInf);
  const uint32_t start = ((begin_ * 64u + begin_) * 64u + begin_) * 64u + begin_;
  for (size_t a = 0; a < n; ++a) {
    if (!lat->run_end_[a])
      continue;
    uint32_t h = start;
    double s = 0;
    const size_t top = (std::min)(lmax, lat->run_end_[a] - a);
    for (size_t len = 1; len <= top; ++len) {
      const uint32_t x = lat->ids_[a + len - 1];
      s += Lp(en_, h, x);
      h = ((h << 6) | x) & kMask;
      if (len >= lmin)
        lat->en_[a * width + len] = s + Lp(en_, h, end_);
    }
  }

  // 前向：每個位置留 beam 個「不在英文段裡」的狀態（中文前文 h），英文段的狀態不用存：
  // 段的分數只看開頭的狀態和段本身，結尾直接回到同一個中文前文
  auto& states = lat->states_;
  states.assign(n + 1, {});
  lat->best_forward_.assign(n + 1, kNegInf);
  std::unordered_map<uint32_t, Lattice::State> cur, nxt;
  cur.reserve(beam * 4);
  nxt.reserve(beam * 4);
  auto put = [](std::unordered_map<uint32_t, Lattice::State>& map,
                const Lattice::State& st, double* top) {
    auto it = map.find(st.h);
    if (it == map.end())
      map.emplace(st.h, st);
    else if (st.score > it->second.score)
      it->second = st;
    else
      return;
    *top = (std::max)(*top, st.score);
  };
  {
    Lattice::State s0{start, 0, 0, kNegInf, -1, -1, -1, -1, false, false};
    cur.emplace(start, s0);
  }
  double top = 0;
  for (size_t pos = 0; pos <= n; ++pos) {
    // 英文段 [a, pos) 結束在這裡
    for (size_t len = lmin; len <= lmax && len <= pos; ++len) {
      const size_t a = pos - len;
      const double e = lat->En(a, len);
      if (e == kNegInf)
        continue;
      const double c = switch_cost + e;
      if (lat->best_forward_[a] + c < top - params.beam_width)
        continue;
      const auto& src = states[a];
      for (size_t j = 0; j < src.size(); ++j) {
        Lattice::State st{src[j].h, src[j].score + c, 0, kNegInf,
                          static_cast<int32_t>(a), static_cast<int32_t>(j),
                          -1, -1, true, false};
        put(cur, st, &top);
      }
    }
    // 剪：差 beam_width 以上的不留，最多 beam 個；照 h 排好給 Find 二分搜尋
    auto& here = states[pos];
    here.reserve(cur.size());
    for (const auto& x : cur) {
      if (x.second.score >= top - params.beam_width)
        here.push_back(x.second);
    }
    cur.clear();
    if (here.size() > beam) {
      std::nth_element(here.begin(), here.begin() + beam, here.end(),
                       [](const Lattice::State& x, const Lattice::State& y) {
                         return x.score > y.score;
                       });
      here.resize(beam);
    }
    std::sort(here.begin(), here.end(),
              [](const Lattice::State& x, const Lattice::State& y) {
                return x.h < y.h;
              });
    lat->best_forward_[pos] = top;
    // 這一鍵當中文
    top = kNegInf;
    for (size_t j = 0; j < here.size(); ++j) {
      auto& st = here[j];
      // 結尾時還是起點的前文＝整串都是英文、沒有中文鍵：不算中文模型的 '$'（空的中文句子幾乎不可能，
      // 會把整段英文扣掉約 40 分，ovh、agent 這種一整段的英文永遠進不了候選）
      st.next = pos < n ? Lp(zh_, st.h, lat->ids_[pos])
                        : (st.h == start ? 0.0 : Lp(zh_, st.h, end_));
      if (pos == n)
        continue;
      Lattice::State to{((st.h << 6) | lat->ids_[pos]) & kMask,
                        st.score + st.next,
                        0,
                        kNegInf,
                        static_cast<int32_t>(pos),
                        static_cast<int32_t>(j),
                        -1,
                        -1,
                        false,
                        false};
      put(nxt, to, &top);
    }
    std::swap(cur, nxt);
    nxt.clear();
  }
  lat->best_ = kNegInf;
  for (const auto& st : states[n])
    lat->best_ = (std::max)(lat->best_, st.score + st.next);

  // 後向：從每個留下來的狀態到結尾的最佳分數（只走留下來的狀態）
  lat->best_backward_.assign(n + 1, kNegInf);
  for (auto& st : states[n]) {
    st.back = st.next;
    lat->best_backward_[n] = (std::max)(lat->best_backward_[n], st.back);
  }
  for (size_t pos = n; pos-- > 0;) {
    for (auto& st : states[pos]) {
      double v = kNegInf;
      int k = lat->Find(pos + 1, ((st.h << 6) | lat->ids_[pos]) & kMask);
      if (k >= 0) {
        v = st.next + states[pos + 1][k].back;
        st.to = static_cast<int32_t>(pos + 1);
        st.to_idx = k;
        st.to_en = false;
      }
      if (lat->run_end_[pos]) {
        const size_t top_len = (std::min)(lmax, lat->run_end_[pos] - pos);
        for (size_t len = lmin; len <= top_len; ++len) {
          const double c = switch_cost + lat->En(pos, len);
          const size_t e = pos + len;
          if (c + lat->best_backward_[e] <= v)
            continue;
          int m = lat->Find(e, st.h);
          if (m < 0)
            continue;
          const double w = c + states[e][m].back;
          if (w > v) {
            v = w;
            st.to = static_cast<int32_t>(e);
            st.to_idx = m;
            st.to_en = true;
          }
        }
      }
      st.back = v;
      lat->best_backward_[pos] = (std::max)(lat->best_backward_[pos], v);
    }
  }

  // 每一段英文的強制分數：開頭的狀態＋這段＋結尾同一個中文前文的狀態
  for (size_t a = 0; a < n; ++a) {
    if (!lat->run_end_[a])
      continue;
    const size_t top_len = (std::min)(lmax, lat->run_end_[a] - a);
    for (size_t len = lmin; len <= top_len; ++len) {
      const double c = switch_cost + lat->En(a, len);
      if (lat->best_forward_[a] + c + lat->best_backward_[a + len] <
          lat->best_ - params.margin)
        continue;
      Span span{a, a + len, 0};
      int idx;
      const double forced = lat->Forced(span, &idx);
      if (forced == kNegInf || forced - lat->best_ < -params.margin)
        continue;
      span.gap = forced - lat->best_;
      lat->spans_.push_back(span);
    }
  }
  std::sort(lat->spans_.begin(), lat->spans_.end(),
            [](const Span& x, const Span& y) {
              if (x.gap != y.gap)
                return x.gap > y.gap;
              return x.start < y.start || (x.start == y.start && x.end < y.end);
            });
  return lat;
}

double EnglishKeylid::Lattice::En(size_t a, size_t len) const {
  if (len >= width_ || a >= input_.size())
    return kNegInf;
  return en_[a * width_ + len];
}

int EnglishKeylid::Lattice::Find(size_t pos, uint32_t h) const {
  const auto& v = states_[pos];
  size_t lo = 0, hi = v.size();
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (v[mid].h < h)
      lo = mid + 1;
    else
      hi = mid;
  }
  return (lo < v.size() && v[lo].h == h) ? static_cast<int>(lo) : -1;
}

double EnglishKeylid::Lattice::Forced(const Span& span, int* idx) const {
  const double c = -params_.switch_penalty + En(span.start, span.length());
  double best = kNegInf;
  *idx = -1;
  if (c == kNegInf)
    return best;
  const auto& src = states_[span.start];
  for (size_t j = 0; j < src.size(); ++j) {
    int k = Find(span.end, src[j].h);
    if (k < 0)
      continue;
    const double v = src[j].score + c + states_[span.end][k].back;
    if (v > best) {
      best = v;
      *idx = static_cast<int>(j);
    }
  }
  return best;
}

double EnglishKeylid::Lattice::Llr(const Span& span) const {
  int idx;
  const double forced = Forced(span, &idx);
  // 強制這段中文：開頭的狀態一鍵一鍵當中文走到結尾（只走留下來的狀態）
  double forbid = kNegInf;
  const auto& src = states_[span.start];
  for (size_t j = 0; j < src.size(); ++j) {
    double s = src[j].score;
    int k = static_cast<int>(j);
    size_t pos = span.start;
    for (; pos < span.end && k >= 0; ++pos) {
      const auto& st = states_[pos][k];
      s += st.next;
      k = Find(pos + 1, ((st.h << 6) | ids_[pos]) & kMask);
    }
    if (k < 0)
      continue;
    forbid = (std::max)(forbid, s + states_[span.end][k].back);
  }
  if (forbid == kNegInf)
    return std::numeric_limits<double>::infinity();
  return forced - forbid;
}

std::vector<std::pair<size_t, size_t>> EnglishKeylid::Lattice::Path(
    const Span& span) const {
  std::vector<std::pair<size_t, size_t>> out;
  int idx;
  if (Forced(span, &idx) == kNegInf)
    return out;
  // 前面：從開頭的狀態往回走
  int pos = static_cast<int>(span.start), k = idx;
  while (pos > 0 && k >= 0) {
    const auto& st = states_[pos][k];
    if (st.from < 0)
      break;
    if (st.from_en)
      out.push_back({static_cast<size_t>(st.from), static_cast<size_t>(pos)});
    k = st.from_idx;
    pos = st.from;
  }
  std::reverse(out.begin(), out.end());
  out.push_back({span.start, span.end});
  // 後面：從結尾同一個中文前文的狀態往後走
  pos = static_cast<int>(span.end);
  k = Find(span.end, states_[span.start][idx].h);
  while (k >= 0 && pos < static_cast<int>(input_.size())) {
    const auto& st = states_[pos][k];
    if (st.to < 0)
      break;
    if (st.to_en)
      out.push_back({static_cast<size_t>(pos), static_cast<size_t>(st.to)});
    k = st.to_idx;
    pos = st.to;
  }
  return out;
}

}  // namespace rime
