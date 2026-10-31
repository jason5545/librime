//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-07-12 Zou Xu <zouivex@gmail.com>
// 2012-02-11 GONG Chen <chen.sst@gmail.com>
//
#include <algorithm>
#include <queue>
#include <boost/range/adaptor/reversed.hpp>
#include <rime/algo/syllabifier.h>
#include <rime/dict/corrector.h>
#include <rime/dict/prism.h>
#include "syllabifier.h"

namespace rime {
using namespace corrector;

using Vertex = pair<size_t, SpellingType>;
using VertexQueue =
    std::priority_queue<Vertex, vector<Vertex>, std::greater<Vertex>>;

// 權重階梯：
// 1. 全拼：用戶完整輸入了所有編碼。Penalty = 0
// 2. 簡拼：用戶輸入了縮寫，明確希望匹配某個字。Penalty ≈ -2.3
// 3. 補全 (Completion)：用戶還沒打完，算法瞎猜的。Penalty ≈ -3.0
const double kCompletionPenalty = -2.995732273553991;      // log(0.05)
const double kCorrectionCredibility = -4.605170185988091;  // log(0.01)

int Syllabifier::BuildSyllableGraph(const string& input,
                                    Prism& prism,
                                    SyllableGraph* graph) {
  if (input.empty())
    return 0;

  size_t farthest = 0;
  VertexQueue queue;
  queue.push(Vertex{0, kNormalSpelling});  // start

  while (!queue.empty()) {
    Vertex vertex(queue.top());
    queue.pop();
    size_t current_pos = vertex.first;

    // record a visit to the vertex
    if (graph->vertices.find(current_pos) == graph->vertices.end()) {
      graph->vertices.insert(vertex);  // preferred spelling type comes first
    } else {
      //      graph->vertices[current_pos] =
      //          std::min(vertex.second, graph->vertices[current_pos]);
      continue;  // discard worse spelling types
    }

    if (current_pos > farthest)
      farthest = current_pos;

    // consume leading delimiters
    size_t begin_pos = current_pos;
    while (begin_pos < input.length() &&
           delimiters_.find(input[begin_pos]) != string::npos)
      ++begin_pos;
    DLOG(INFO) << "current_pos: " << current_pos
               << ", begin_pos: " << begin_pos;

    // see where we can go by advancing a syllable
    vector<Prism::Match> matches;
    set<SyllableId> exact_match_syllables;
    auto current_input = input.substr(begin_pos);
    prism.CommonPrefixSearch(current_input, &matches);
    if (corrector_) {
      for (auto& m : matches) {
        exact_match_syllables.insert(m.value);
      }
      Corrections corrections;
      corrector_->ToleranceSearch(prism, current_input, &corrections, 5);
      for (const auto& m : corrections) {
        for (auto accessor = prism.QuerySpelling(m.first);
             !accessor.exhausted(); accessor.Next()) {
          auto props = accessor.properties();
          if (props.type == kNormalSpelling && !props.is_correction) {
            matches.push_back({m.first, m.second.length});
            break;
          }
        }
      }
    }

    size_t leading_gap = begin_pos - current_pos;
    if (!matches.empty()) {
      auto& end_vertices(graph->edges[current_pos]);
      for (const auto& m : matches) {
        if (m.length == 0)
          continue;
        size_t end_pos = current_pos + leading_gap + m.length;
        // consume trailing delimiters
        while (end_pos < input.length() &&
               delimiters_.find(input[end_pos]) != string::npos)
          ++end_pos;
        DLOG(INFO) << "end_pos: " << end_pos;
        bool matches_input = (current_pos == 0 && end_pos == input.length());
        SpellingMap& spellings(end_vertices[end_pos]);
        SpellingType end_vertex_type = kInvalidSpelling;
        // when spelling algebra is enabled,
        // a spelling evaluates to a set of syllables;
        // otherwise, it resembles exactly the syllable itself.
        SpellingAccessor accessor(prism.QuerySpelling(m.value));
        while (!accessor.exhausted()) {
          SyllableId syllable_id = accessor.syllable_id();
          EdgeProperties props(accessor.properties());
          if (strict_spelling_ && matches_input &&
              props.type != kNormalSpelling) {
            // disqualify fuzzy spelling or abbreviation as single word
          } else {
            props.end_pos = end_pos;
            // add a syllable with properties to the edge's
            // spelling-to-syllable map
            if (corrector_ && exact_match_syllables.find(m.value) ==
                                  exact_match_syllables.end()) {
              props.is_correction = true;
              props.credibility = kCorrectionCredibility;
            }
            // 蓬萊：音節多半照編號由小到大來，比最後一個大就直接接在後面（flat_map）
            if (spellings.empty() || spellings.rbegin()->first < syllable_id) {
              spellings.emplace_hint(spellings.end(), syllable_id, props);
            } else {
              auto it = spellings.find(syllable_id);
              if (it == spellings.end()) {
                spellings.insert({syllable_id, props});
              } else {
                it->second.type = (std::min)(it->second.type, props.type);
              }
            }
            // let end_vertex_type be the best (smaller) type of spelling
            // that ends at the vertex
            if (end_vertex_type > props.type && !props.is_correction) {
              end_vertex_type = props.type;
            }
          }
          accessor.Next();
        }
        if (spellings.empty()) {
          DLOG(INFO) << "not spelled.";
          end_vertices.erase(end_pos);
          continue;
        }
        // find the best common type in a path up to the end vertex
        // eg. pinyin "shurfa" has vertex type kNormalSpelling at position 3,
        // kAbbreviation at position 4 and kAbbreviation at position 6
        if (end_vertex_type < vertex.second) {
          end_vertex_type = vertex.second;
        }
        queue.push(Vertex{end_pos, end_vertex_type});
        DLOG(INFO) << "added to syllable graph, edge: [" << current_pos << ", "
                   << end_pos << ")";
      }
    }
  }

  DLOG(INFO) << "remove stale vertices and edges";
  set<int> good;
  good.insert(farthest);
  // fuzzy spellings are immune to invalidation by normal spellings
  SpellingType last_type =
      (std::max)(graph->vertices[farthest], kFuzzySpelling);
  for (int i = farthest - 1; i >= 0; --i) {
    if (graph->vertices.find(i) == graph->vertices.end())
      continue;
    // remove stale edges
    for (auto j = graph->edges[i].begin(); j != graph->edges[i].end();) {
      if (good.find(j->first) == good.end()) {
        // not connected
        graph->edges[i].erase(j++);
        continue;
      }
      // remove disqualified syllables (eg. matching abbreviated spellings)
      // when there is a path of more favored type
      SpellingType edge_type = kInvalidSpelling;
      // 蓬萊：SpellingMap 是 flat_map，一個一個 erase 每次都要搬後面的（按下聲調鍵時簡拼整批刪掉）。
      // 判斷照原本的順序做，留下來的照順序搬到前面，最後一次刪掉
      size_t kept = 0;
      for (auto k = j->second.begin(); k != j->second.end(); ++k) {
        bool keep = true;
        if (k->second.is_correction) {
          // Don't care correction edges
        } else if (k->second.type > last_type) {
          keep = false;
        } else if (k->second.type < edge_type) {
          edge_type = k->second.type;
        }
        if (keep) {
          auto dest = j->second.begin() + kept;
          if (dest != k)
            *dest = boost::move(*k);
          ++kept;
        }
      }
      j->second.erase(j->second.begin() + kept, j->second.end());
      if (j->second.empty()) {
        graph->edges[i].erase(j++);
      } else {
        if (edge_type < kAbbreviation)
          CheckOverlappedSpellings(graph, i, j->first);
        ++j;
      }
    }
    if (graph->vertices[i] > last_type || graph->edges[i].empty()) {
      DLOG(INFO) << "remove stale vertex at " << i;
      graph->vertices.erase(i);
      graph->edges.erase(i);
      continue;
    }
    // keep the valid vertex
    good.insert(i);
  }

  if (enable_completion_ && farthest < input.length()) {
    DLOG(INFO) << "completion enabled";
    const size_t kExpandSearchLimit = 512;
    vector<Prism::Match> keys;
    prism.ExpandSearch(input.substr(farthest), &keys, kExpandSearchLimit);
    if (!keys.empty()) {
      size_t current_pos = farthest;
      size_t end_pos = input.length();
      size_t code_length = end_pos - current_pos;
      auto& end_vertices(graph->edges[current_pos]);
      auto& spellings(end_vertices[end_pos]);
      // 蓬萊：最多 512 個鍵、音節順序不一定，flat_map 逐一插入會一直搬。先照原本的順序收集，
      // 照音節編號穩定排序、同一個音節留第一個（原本 insert 已經有的不換），再照順序放進去
      vector<pair<SyllableId, SpellingProperties>> collected;
      for (const auto& m : keys) {
        if (m.length < code_length)
          continue;
        // when spelling algebra is enabled,
        // a spelling evaluates to a set of syllables;
        // otherwise, it resembles exactly the syllable itself.
        SpellingAccessor accessor(prism.QuerySpelling(m.value));
        while (!accessor.exhausted()) {
          SyllableId syllable_id = accessor.syllable_id();
          SpellingProperties props = accessor.properties();
          if (props.type < kAbbreviation) {
            props.type = kCompletion;
            props.credibility += kCompletionPenalty;
            props.end_pos = end_pos;
            // add a syllable with properties to the edge's
            // spelling-to-syllable map
            collected.emplace_back(syllable_id, props);
          }
          accessor.Next();
        }
      }
      std::stable_sort(collected.begin(), collected.end(),
                       [](const pair<SyllableId, SpellingProperties>& a,
                          const pair<SyllableId, SpellingProperties>& b) {
                         return a.first < b.first;
                       });
      for (size_t c = 0; c < collected.size(); ++c) {
        if (c > 0 && collected[c].first == collected[c - 1].first)
          continue;
        spellings.insert(spellings.end(),
                         {collected[c].first, collected[c].second});
      }
      if (spellings.empty()) {
        DLOG(INFO) << "no completion could be made.";
        end_vertices.erase(end_pos);
      } else {
        DLOG(INFO) << "added to syllable graph, completion: [" << current_pos
                   << ", " << end_pos << ")";
        farthest = end_pos;
      }
    }
  }

  graph->input_length = input.length();
  graph->interpreted_length = farthest;
  DLOG(INFO) << "input length: " << graph->input_length;
  DLOG(INFO) << "syllabified length: " << graph->interpreted_length;

  Transpose(graph);

  return farthest;
}

void Syllabifier::CheckOverlappedSpellings(SyllableGraph* graph,
                                           size_t start,
                                           size_t end) {
  if (!graph || graph->edges.find(start) == graph->edges.end())
    return;
  // if "Z" = "YX", mark the vertex between Y and X an ambiguous syllable joint
  auto& y_end_vertices(graph->edges[start]);
  // enumerate Ys
  for (const auto& y : y_end_vertices) {
    size_t joint = y.first;
    if (joint >= end)
      break;
    // test X
    if (graph->edges.find(joint) == graph->edges.end())
      continue;
    auto& x_end_vertices(graph->edges[joint]);
    for (auto& x : x_end_vertices) {
      if (x.first < end)
        continue;
      if (x.first == end) {
        // discourage syllables at an ambiguous joint
        // bad cases include pinyin syllabification "niju'ede"
        for (auto& spelling : x.second) {
          // 這條邊（X）相對於起點構成歧義
          spelling.second.ambiguous_source_positions.insert(start);
        }
        graph->vertices[joint] = kAmbiguousSpelling;
        DLOG(INFO) << "ambiguous syllable joint at position " << joint << ".";
      }
      break;
    }
  }
}

void Syllabifier::Transpose(SyllableGraph* graph) {
  // 蓬萊：SpellingIndex 是連續的陣列。原本照「終點由大到小、每個終點裡音節由小到大」逐一
  // index[syll_id].push_back；每個終點的音節本來就排好了，這裡合併這幾串（同一個音節先拿排在前面的終點），
  // 每個音節的邊順序跟原本相同
  using Run = pair<SpellingMap::const_iterator, SpellingMap::const_iterator>;
  vector<Run> runs;
  vector<pair<SyllableId, const EdgeProperties*>> existing;
  for (const auto& start : graph->edges) {
    auto& index(graph->indices[start.first]);
    // 圖裡已經有的放前面（原本是接在後面 push_back）；只有同一張圖建兩次才會有
    existing.clear();
    for (const auto& x : index)
      for (auto props : x.second)
        existing.emplace_back(x.first, props);
    runs.clear();
    size_t total = existing.size();
    for (const auto& end : boost::adaptors::reverse(start.second)) {
      runs.emplace_back(end.second.begin(), end.second.end());
      total += end.second.size();
    }
    index.Reset(total);
    size_t e = 0;
    while (true) {
      int best = -1;
      SyllableId best_id = 0;
      if (e < existing.size()) {
        best = static_cast<int>(runs.size());
        best_id = existing[e].first;
      }
      for (size_t i = 0; i < runs.size(); ++i) {
        if (runs[i].first == runs[i].second)
          continue;
        // 同一個音節：已經有的、排在前面的終點先（嚴格小於才換）
        if (best < 0 || runs[i].first->first < best_id) {
          best = static_cast<int>(i);
          best_id = runs[i].first->first;
        }
      }
      if (best < 0)
        break;
      if (best == static_cast<int>(runs.size())) {
        index.Append(existing[e].first, existing[e].second);
        ++e;
      } else {
        index.Append(best_id, &runs[best].first->second);
        ++runs[best].first;
      }
    }
  }
}

void Syllabifier::EnableCorrection(Corrector* corrector) {
  corrector_ = corrector;
}

}  // namespace rime
