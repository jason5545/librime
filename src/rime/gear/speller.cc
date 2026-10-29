//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-10-27 GONG Chen <chen.sst@gmail.com>
//
#include <utility>
#include <rime/candidate.h>
#include <rime/common.h>
#include <rime/composition.h>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/key_event.h>
#include <rime/key_table.h>
#include <rime/menu.h>
#include <rime/schema.h>
#include <rime/gear/speller.h>

static const char kRimeAlphabet[] = "zyxwvutsrqponmlkjihgfedcba";

namespace rime {

static inline bool belongs_to(char ch, const string& charset) {
  return charset.find(ch) != string::npos;
}

static bool reached_max_code_length(const an<Candidate>& cand,
                                    int max_code_length) {
  if (!cand)
    return false;
  int code_length = static_cast<int>(cand->end() - cand->start());
  return code_length >= max_code_length;
}

static inline bool is_table_entry(const an<Candidate>& cand) {
  const auto& type = Candidate::GetGenuineCandidate(cand)->type();
  return type == "table" || type == "user_table";
}

static inline bool is_simple_candidate(const an<Candidate>& cand) {
  auto genuine = Candidate::GetGenuineCandidate(cand);
  return bool(As<SimpleCandidate>(genuine));
}

static bool is_auto_selectable(const an<Candidate>& cand,
                               const string& input,
                               const string& delimiters) {
  return
      // reaches end of input
      cand->end() == input.length() &&
      (is_table_entry(cand) || is_simple_candidate(cand)) &&
      // no delimiters
      input.find_first_of(delimiters, cand->start()) == string::npos;
}

static bool expecting_an_initial(Context* ctx,
                                 const string& alphabet,
                                 const string& finals) {
  size_t caret_pos = ctx->caret_pos();
  if (caret_pos == 0 ||
      caret_pos == ctx->composition().GetCurrentStartPosition()) {
    return true;
  }
  const string& input(ctx->input());
  char previous_char = input[caret_pos - 1];
  return belongs_to(previous_char, finals) ||
         !belongs_to(previous_char, alphabet);
}

// 蓬萊：整段數字鍵（speller/digit_tones）
static inline bool is_digit(int ch) {
  return ch >= '0' && ch <= '9';
}

static bool all_digits(const string& input) {
  for (char c : input) {
    if (!is_digit(c))
      return false;
  }
  return !input.empty();
}

// 組字區只有一段、從頭開始，蓋到 end（跟 translator/digit_runs 一樣只看整段輸入）
static Segment* whole_segment(Context* ctx, size_t end) {
  Composition& comp = ctx->composition();
  if (comp.empty() || comp.back().start != 0 || comp.back().end != end)
    return nullptr;
  return &comp.back();
}

// 選單裡有蓋住整段的數字候選（translator/digit_runs 給的，型別 digits）
static bool has_whole_digit_candidate(Context* ctx, size_t end) {
  Segment* seg = whole_segment(ctx, end);
  if (!seg || !seg->menu)
    return false;
  // 沒排第一的數字在第 english_letter_position（6）個左右
  const size_t kLookAhead = 20;
  size_t n = seg->menu->Prepare(kLookAhead);
  for (size_t i = 0; i < n; ++i) {
    auto cand = seg->menu->GetCandidateAt(i);
    if (!cand || cand->start() != 0 || cand->end() != end)
      continue;
    for (const auto& item : Candidate::GetGenuineCandidates(cand)) {
      if (item->type() == "digits")
        return true;
    }
  }
  return false;
}

Speller::Speller(const Ticket& ticket)
    : Processor(ticket), alphabet_(kRimeAlphabet) {
  if (Config* config = engine_->schema()->config()) {
    config->GetString("speller/alphabet", &alphabet_);
    config->GetString("speller/delimiter", &delimiters_);
    config->GetString("speller/initials", &initials_);
    config->GetString("speller/finals", &finals_);
    config->GetInt("speller/max_code_length", &max_code_length_);
    config->GetBool("speller/auto_select", &auto_select_);
    config->GetBool("speller/use_space", &use_space_);
    string pattern;
    if (config->GetString("speller/auto_select_pattern", &pattern)) {
      auto_select_pattern_ = pattern;
    }
    string auto_clear;
    if (config->GetString("speller/auto_clear", &auto_clear)) {
      if (auto_clear == "auto")
        auto_clear_ = kClearAuto;
      else if (auto_clear == "manual")
        auto_clear_ = kClearManual;
      else if (auto_clear == "max_length")
        auto_clear_ = kClearMaxLength;
    }
    // 蓬萊：整段都是數字鍵時，連著的聲調鍵也收（0912345678 的 4、7）。只在翻譯器出得了數字候選時收
    // （translator/digit_runs，見 AcceptsDigitTone），不然收進來也只剩中文切不開的半句
    config->GetBool("speller/digit_tones", &digit_tones_);
    // 開頭是聲調鍵的數字收成一段（abc_segmentor）：開頭的聲調鍵不算連著的，後面的空白照舊是一聲
    config->GetBool("speller/digit_lead_tones", &digit_lead_tones_);
  }
  if (initials_.empty()) {
    initials_ = alphabet_;
  }
}

ProcessResult Speller::ProcessKeyEvent(const KeyEvent& key_event) {
  if (key_event.release() || key_event.ctrl() || key_event.alt() ||
      key_event.super())
    return kNoop;
  int ch = key_event.keycode();
  if (ch < 0x20 || ch >= 0x7f)  // not a valid key for spelling
    return kNoop;
  if (ch == XK_space && (!use_space_ || key_event.shift()))
    return kNoop;
  if (!belongs_to(ch, alphabet_) && !belongs_to(ch, delimiters_))
    return kNoop;
  Context* ctx = engine_->context();
  // 蓬萊：收過連著的聲調鍵的數字串不可能是中文，空白鍵不當一聲，交給後面確認選字（跟 ㄋㄧˇ＋空白一樣）
  if (ch == XK_space && InDigitToneRun(ctx))
    return kNoop;
  bool is_initial = belongs_to(ch, initials_);
  if (!is_initial && expecting_an_initial(ctx, alphabet_, finals_) &&
      !AcceptsDigitTone(ctx, ch)) {
    return kNoop;
  }
  // handles input beyond max_code_length when auto_select is false.
  if (is_initial && AutoSelectAtMaxCodeLength(ctx)) {
    DLOG(INFO) << "auto-select at max code length.";
  } else if ((auto_clear_ == kClearMaxLength || auto_clear_ == kClearManual) &&
             !KeepsDigitLead(ctx, ch) && AutoClear(ctx)) {
    DLOG(INFO) << "auto-clear at max code when no candidate.";
  }
  // make a backup of previous conversion before modifying input
  Segment previous_segment;
  if (auto_select_ && ctx->HasMenu()) {
    previous_segment = ctx->composition().back();
  }
  DLOG(INFO) << "add to input: '" << (char)ch << "', " << key_event.repr();
  ctx->PushInput(ch);
  ctx->BeginEditing();
  if (AutoSelectPreviousMatch(ctx, &previous_segment)) {
    DLOG(INFO) << "auto-select previous match.";
    // after auto-selecting, if only the current non-initial key is left,
    // then it should be handled by other processors.
    if (!is_initial && ctx->composition().GetCurrentSegmentLength() == 1) {
      ctx->PopInput(1);
      return kNoop;
    }
  }
  if (AutoSelectUniqueCandidate(ctx)) {
    DLOG(INFO) << "auto-select unique candidate.";
  } else if (auto_clear_ == kClearAuto && AutoClear(ctx)) {
    DLOG(INFO) << "auto-clear when no candidate.";
  }
  return kAccepted;
}

// 聲調鍵接在聲調鍵（或空白）後面、本來不收的時候：整段輸入都是數字鍵、組字區只有一段，
// 而且選單裡有蓋住整段的數字候選，才收進輸入。組字區空的、聲調鍵開頭（caret 0）照舊不收；
// 整段剛好是一個詞時翻譯器不出數字候選，照舊交給選字（打完 ㄞˋ 多按一次 ˋ 還是選第 4 個）
bool Speller::AcceptsDigitTone(Context* ctx, int ch) {
  if (!digit_tones_ || !is_digit(ch))
    return false;
  const string& input = ctx->input();
  const size_t caret = ctx->caret_pos();
  if (caret == 0 || caret != input.length() || !all_digits(input))
    return false;
  return has_whole_digit_candidate(ctx, caret);
}

// 蓬萊（speller/digit_lead_tones）：開頭的聲調鍵單獨成一段 raw、翻譯器加了 digit_lead（出得了數字），接著按數字鍵時
// 不 AutoClear：那段沒有選單是因為還沒接數字，接上之後 abc_segmentor 會收成一段（auto_clear: manual 時 30 不會剩 0）
bool Speller::KeepsDigitLead(Context* ctx, int ch) {
  if (!digit_lead_tones_ || !is_digit(ch) || belongs_to(ch, finals_))
    return false;
  const string& input = ctx->input();
  const Composition& comp = ctx->composition();
  return ctx->caret_pos() == input.length() && !comp.empty() &&
         comp.back().start == 0 && comp.back().end == input.length() &&
         comp.back().HasTag("raw") && comp.back().HasTag("digit_lead");
}

// 整段輸入都是數字鍵、組字區只有一段，而且裡面有連著的聲調鍵（只有 AcceptsDigitTone 收得進來）
bool Speller::InDigitToneRun(Context* ctx) {
  if (!digit_tones_)
    return false;
  const string& input = ctx->input();
  if (ctx->caret_pos() != input.length() || !all_digits(input) ||
      !whole_segment(ctx, input.length()))
    return false;
  size_t i = 1;
  // 蓬萊（speller/digit_lead_tones）：3ˇ4ˋ0 的 34 是開頭的聲調鍵，不是 AcceptsDigitTone 收進來的
  while (digit_lead_tones_ && i < input.length() &&
         belongs_to(input[i - 1], finals_))
    ++i;
  for (; i < input.length(); ++i) {
    if (belongs_to(input[i], finals_) && belongs_to(input[i - 1], finals_))
      return true;
  }
  return false;
}

bool Speller::AutoSelectAtMaxCodeLength(Context* ctx) {
  if (max_code_length_ <= 0)
    return false;
  if (!ctx->HasMenu())
    return false;
  auto cand = ctx->GetSelectedCandidate();
  if (cand && reached_max_code_length(cand, max_code_length_) &&
      is_auto_selectable(cand, ctx->input(), delimiters_)) {
    ctx->ConfirmCurrentSelection();
    return true;
  }
  return false;
}

bool Speller::AutoSelectUniqueCandidate(Context* ctx) {
  if (!auto_select_)
    return false;
  if (!ctx->HasMenu())
    return false;
  const Segment& seg(ctx->composition().back());
  bool unique_candidate = seg.menu->Prepare(2) == 1;
  if (!unique_candidate)
    return false;
  const string& input(ctx->input());
  auto cand = seg.GetSelectedCandidate();
  bool matches_input_pattern = false;
  if (auto_select_pattern_.empty()) {
    matches_input_pattern =
        max_code_length_ == 0 ||  // match any length if not set
        reached_max_code_length(cand, max_code_length_);
  } else {
    string code(input.substr(cand->start(), cand->end()));
    matches_input_pattern = boost::regex_match(code, auto_select_pattern_);
  }
  if (matches_input_pattern && is_auto_selectable(cand, input, delimiters_)) {
    ctx->ConfirmCurrentSelection();
    return true;
  }
  return false;
}

bool Speller::AutoSelectPreviousMatch(Context* ctx, Segment* previous_segment) {
  if (!auto_select_)
    return false;
  if (max_code_length_ > 0 || !auto_select_pattern_.empty())
    return false;
  if (ctx->HasMenu())  // if and only if current conversion fails
    return false;
  if (!previous_segment->menu)
    return false;
  size_t start = previous_segment->start;
  size_t end = previous_segment->end;
  string input = ctx->input();
  string converted = input.substr(0, end);
  if (is_auto_selectable(previous_segment->GetSelectedCandidate(), converted,
                         delimiters_)) {
    // reuse previous match
    ctx->composition().pop_back();
    ctx->composition().push_back(std::move(*previous_segment));
    ctx->ConfirmCurrentSelection();
    if (ctx->get_option("_auto_commit")) {
      ctx->set_input(converted);
      ctx->Commit();
      string rest = input.substr(end);
      ctx->set_input(rest);
    }
    return true;
  }
  return FindEarlierMatch(ctx, start, end);
}

bool Speller::AutoClear(Context* ctx) {
  if (!ctx->HasMenu() && auto_clear_ > kClearNone &&
      (auto_clear_ != kClearMaxLength || max_code_length_ == 0 ||
       ctx->input().length() >= (size_t)max_code_length_)) {
    ctx->Clear();
    return true;
  }
  return false;
}

bool Speller::FindEarlierMatch(Context* ctx, size_t start, size_t end) {
  if (end <= start + 1)
    return false;
  string input = ctx->input();
  string converted = input;
  while (--end > start) {
    converted.resize(end);
    ctx->set_input(converted);
    if (!ctx->HasMenu())
      break;
    const Segment& segment(ctx->composition().back());
    if (is_auto_selectable(segment.GetSelectedCandidate(), converted,
                           delimiters_)) {
      // select previous match
      if (ctx->get_option("_auto_commit")) {
        ctx->Commit();
        string rest = input.substr(end);
        ctx->set_input(rest);
        end = 0;
      } else {
        ctx->ConfirmCurrentSelection();
        ctx->set_input(input);
      }
      if (!ctx->HasMenu()) {
        size_t next_start = ctx->composition().GetCurrentStartPosition();
        size_t next_end = ctx->composition().GetCurrentEndPosition();
        if (next_start == end) {
          // continue splitting
          FindEarlierMatch(ctx, next_start, next_end);
        }
      }
      return true;
    }
  }
  ctx->set_input(input);
  return false;
}

}  // namespace rime
