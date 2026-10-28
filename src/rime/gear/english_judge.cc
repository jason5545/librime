//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 蓬萊：中英混輸的判斷（translator/english_judge）
//
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <rime/gear/english_judge.h>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace rime {

namespace {

const char kMagic[8] = {'P', 'L', 'J', 'U', 'D', 'G', 'E', '2'};
const size_t kSections = 15;
const size_t kEnSize = 28 * 28 * 28;

// 漢字：U+3400–4DBF、U+4E00–9FFF、U+F900–FAFF（跟 judge3.py 的 HAN 相同）
inline bool is_han(uint32_t cp) {
  return (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) ||
         (cp >= 0xF900 && cp <= 0xFAFF);
}

inline bool is_letter(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// 解一個 UTF-8 字元；不合法的位元組當成一個非漢字
uint32_t decode(const std::string& s, size_t* i) {
  unsigned char c = s[*i];
  if (c < 0x80) {
    ++*i;
    return c;
  }
  int n = (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : 0;
  if (n == 0 || *i + n > s.size() - 1) {
    ++*i;
    return 0xFFFD;
  }
  uint32_t cp = c & (0x3F >> n);
  for (int k = 1; k <= n; ++k) {
    unsigned char x = s[*i + k];
    if ((x >> 6) != 2) {
      ++*i;
      return 0xFFFD;
    }
    cp = (cp << 6) | (x & 0x3F);
  }
  *i += n + 1;
  return cp;
}

template <class T>
inline long find_in(const T* keys, size_t lo, size_t hi, uint32_t q) {
  const T* p = std::lower_bound(keys + lo, keys + hi, q);
  return (p != keys + hi && *p == q) ? static_cast<long>(p - keys) : -1;
}

}  // namespace

std::shared_ptr<EnglishJudge> EnglishJudge::Open(const std::string& file,
                                                 std::string* error) {
  std::shared_ptr<EnglishJudge> judge(new EnglishJudge);
#ifdef _WIN32
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    *error = "cannot open";
    return nullptr;
  }
  judge->owned_.assign(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
  judge->base_ = &judge->owned_[0];
  judge->size_ = judge->owned_.size();
#else
  int fd = ::open(file.c_str(), O_RDONLY);
  if (fd < 0) {
    *error = "cannot open";
    return nullptr;
  }
  struct stat st;
  if (::fstat(fd, &st) != 0 || st.st_size < 24) {
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
  judge->base_ = base;
  judge->size_ = st.st_size;
  judge->mapped_ = true;
#endif
  const char* p = static_cast<const char*>(judge->base_);
  const size_t header = 24 + 16 * kSections;
  if (judge->size_ < header || std::memcmp(p, kMagic, 8) != 0) {
    *error = "not a judge table (magic)";
    return nullptr;
  }
  uint64_t nb;
  std::memcpy(&judge->d_, p + 8, 8);
  std::memcpy(&nb, p + 16, 8);
  judge->nb_ = static_cast<double>(nb);
  const size_t widths[kSections] = {4, 4, 4, 4, 4, 2, 4, 4, 4, 4, 2, 4, 4, 8, 1};
  const void* at[kSections];
  uint64_t count[kSections];
  for (size_t i = 0; i < kSections; ++i) {
    uint64_t off;
    std::memcpy(&off, p + 24 + 16 * i, 8);
    std::memcpy(&count[i], p + 32 + 16 * i, 8);
    if (off % 8 != 0 || off > judge->size_ ||
        count[i] > (judge->size_ - off) / widths[i]) {
      *error = "bad section " + std::to_string(i);
      return nullptr;
    }
    at[i] = p + off;
  }
  if (count[0] != count[1] || count[2] != count[3] ||
      count[4] != count[2] + 1 || count[5] != count[6] ||
      count[7] != count[8] || count[9] != count[7] + 1 ||
      count[10] != count[11] || count[12] % 2 != 0 || count[13] != kEnSize) {
    *error = "inconsistent section sizes";
    return nullptr;
  }
  judge->uni_key_ = static_cast<const uint32_t*>(at[0]);
  judge->uni_cont_ = static_cast<const uint32_t*>(at[1]);
  judge->b_key_ = static_cast<const uint32_t*>(at[2]);
  judge->b_cont_ = static_cast<const uint32_t*>(at[3]);
  judge->b_off_ = static_cast<const uint32_t*>(at[4]);
  judge->bc_c_ = static_cast<const uint16_t*>(at[5]);
  judge->bc_cont_ = static_cast<const uint32_t*>(at[6]);
  judge->ab_key_ = static_cast<const uint32_t*>(at[7]);
  judge->ab_cnt_ = static_cast<const uint32_t*>(at[8]);
  judge->ab_off_ = static_cast<const uint32_t*>(at[9]);
  judge->tri_c_ = static_cast<const uint16_t*>(at[10]);
  judge->tri_cnt_ = static_cast<const uint32_t*>(at[11]);
  judge->map_ = static_cast<const uint32_t*>(at[12]);
  judge->en_ = static_cast<const double*>(at[13]);
  judge->n_uni_ = count[0];
  judge->n_b_ = count[2];
  judge->n_ab_ = count[7];
  judge->n_map_ = count[12] / 2;
  if (judge->b_off_[judge->n_b_] != count[5] ||
      judge->ab_off_[judge->n_ab_] != count[10]) {
    *error = "bad offsets";
    return nullptr;
  }
  return judge;
}

EnglishJudge::~EnglishJudge() {
#ifndef _WIN32
  if (mapped_ && base_)
    ::munmap(base_, size_);
#endif
}

uint32_t EnglishJudge::Normalize(uint32_t cp) const {
  size_t lo = 0, hi = n_map_;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (map_[2 * mid] < cp)
      lo = mid + 1;
    else
      hi = mid;
  }
  if (lo < n_map_ && map_[2 * lo] == cp)
    cp = map_[2 * lo + 1];
  // 表裡的字都在 16 位元內；超過的當成沒見過的字（U+FFFF 不會出現在語料）
  return cp > 0xFFFF ? 0xFFFF : cp;
}

// 同 judge3.py 的 logp_batch（numpy 的運算順序照抄，分數逐位相同到小數點後 4 位）
double EnglishJudge::ChineseLogp(uint32_t a, uint32_t b, uint32_t c) const {
  long i = find_in(uni_key_, 0, n_uni_, c);
  double cc = i >= 0 ? uni_cont_[i] : 0;
  double p1 = cc > 0 ? cc / nb_ : 0.5 / nb_;
  double p2 = p1;
  i = find_in(b_key_, 0, n_b_, b);
  if (i >= 0) {
    size_t lo = b_off_[i], hi = b_off_[i + 1];
    long j = find_in(bc_c_, lo, hi, c);
    double nbc = j >= 0 ? bc_cont_[j] : 0;
    double cont = (std::max)(static_cast<double>(b_cont_[i]), 1.0);
    p2 = (std::max)(nbc - d_, 0.0) / cont +
         d_ * static_cast<double>(hi - lo) / cont * p1;
  }
  double p3 = p2;
  i = find_in(ab_key_, 0, n_ab_, (a << 16) | b);
  if (i >= 0) {
    size_t lo = ab_off_[i], hi = ab_off_[i + 1];
    long j = find_in(tri_c_, lo, hi, c);
    double n3 = j >= 0 ? tri_cnt_[j] : 0;
    double n = (std::max)(static_cast<double>(ab_cnt_[i]), 1.0);
    p3 = (std::max)(n3 - d_, 0.0) / n +
         d_ * static_cast<double>(hi - lo) / n * p2;
  }
  return std::log(p3);
}

double EnglishJudge::EnglishLogp(const std::string& word) const {
  int h1 = 0, h2 = 0;
  double lp = 0;
  for (size_t k = 0; k <= word.size(); ++k) {
    int ch = 27;  // $
    if (k < word.size()) {
      char c = word[k];
      ch = (c >= 'A' && c <= 'Z') ? c - 'A' + 1 : c - 'a' + 1;
      if (ch < 1 || ch > 26)
        continue;
    }
    lp += en_[(h1 * 28 + h2) * 28 + ch];
    h1 = h2;
    h2 = ch;
  }
  return lp;
}

double EnglishJudge::Score(const std::string& text) const {
  double zh = 0, en = 0;
  uint32_t h1 = 0, h2 = 0;
  auto stop = [&]() {
    if (h2 != 0)
      zh += ChineseLogp(h1, h2, 0);
    h1 = h2 = 0;
  };
  for (size_t i = 0; i < text.size();) {
    if (is_letter(text[i])) {
      size_t j = i;
      while (j < text.size() && is_letter(text[j]))
        ++j;
      stop();
      en += EnglishLogp(text.substr(i, j - i));
      i = j;
      continue;
    }
    uint32_t cp = decode(text, &i);
    if (is_han(cp)) {
      uint32_t c = Normalize(cp);
      zh += ChineseLogp(h1, h2, c);
      h1 = h2;
      h2 = c;
    } else {
      stop();
    }
  }
  stop();
  return en + zh;
}

}  // namespace rime
