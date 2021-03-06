/* Copyright 2016 Google Inc. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#ifndef TENSORFLOW_CORE_UTIL_CTC_CTC_BEAM_SEARCH_H_
#define TENSORFLOW_CORE_UTIL_CTC_CTC_BEAM_SEARCH_H_

#include <cmath>
#include <memory>

#include "third_party/eigen3/Eigen/Core"
#include "tensorflow/core/lib/gtl/top_n.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/macros.h"
#include "tensorflow/core/platform/types.h"
#include "tensorflow/core/util/ctc/ctc_beam_entry.h"
#include "tensorflow/core/util/ctc/ctc_beam_scorer.h"
#include "tensorflow/core/util/ctc/ctc_decoder.h"
#include "tensorflow/core/util/ctc/ctc_loss_util.h"

namespace tensorflow {
namespace ctc {

template <typename CTCBeamState = ctc_beam_search::EmptyBeamState,
          class CTCBeamScorer = BaseBeamScorer<CTCBeamState>,
          typename CTCBeamComparer =
              ctc_beam_search::BeamComparer<CTCBeamState>>
class CTCBeamSearchDecoder : public CTCDecoder {
  // Beam Search
  //
  // Example (GravesTh Fig. 7.5):
  //         a    -
  //  P = [ 0.3  0.7 ]  t = 0
  //      [ 0.4  0.6 ]  t = 1
  //
  // Then P(l = -) = P(--) = 0.7 * 0.6 = 0.42
  //      P(l = a) = P(a-) + P(aa) + P(-a) = 0.3*0.4 + ... = 0.58
  //
  // In this case, Best Path decoding is suboptimal.
  //
  // For Beam Search, we use the following main recurrence relations:
  //
  // Relation 1:
  // ---------------------------------------------------------- Eq. 1
  //      P(l=abcd @ t=7) = P(l=abc  @ t=6) * P(d @ 7)
  //                      + P(l=abcd @ t=6) * (P(d @ 7) + P(- @ 7))
  // where P(l=? @ t=7), ? = a, ab, abc, abcd are all stored and
  // updated recursively in the beam entry.
  //
  // Relation 2:
  // ---------------------------------------------------------- Eq. 2
  //      P(l=abc? @ t=3) = P(l=abc @ t=2) * P(? @ 3)
  // for ? in a, b, d, ..., (not including c or the blank index),
  // and the recurrence starts from the beam entry for P(l=abc @ t=2).
  //
  // For this case, the length of the new sequence equals t+1 (t
  // starts at 0).  This special case can be calculated as:
  //   P(l=abc? @ t=3) = P(a @ 0)*P(b @ 1)*P(c @ 2)*P(? @ 3)
  // but we calculate it recursively for speed purposes.
  typedef ctc_beam_search::BeamEntry<CTCBeamState> BeamEntry;
  typedef ctc_beam_search::BeamProbability BeamProbability;

 public:
  CTCBeamSearchDecoder(int num_classes, int beam_width)
      : CTCDecoder(num_classes, 1, false),
        beam_width_(beam_width),
        leaves_(beam_width),
        beam_scorer_(new CTCBeamScorer) {
    Reset();
  }

  CTCBeamSearchDecoder(int num_classes, int beam_width, int batch_size,
                       bool merge_repeated)
      : CTCDecoder(num_classes, batch_size, merge_repeated),
        beam_width_(beam_width),
        leaves_(beam_width),
        beam_scorer_(new CTCBeamScorer) {}

  ~CTCBeamSearchDecoder() override {}

  // Run the hibernating beam search algorithm on the given input.
  void Decode(const CTCDecoder::SequenceLength& seq_len,
              const std::vector<CTCDecoder::Input>& input,
              std::vector<CTCDecoder::Output>* output,
              CTCDecoder::ScoreOutput* scores) override;

  // Calculate the next step of the beam search and update the internal state.
  template <typename Vector>
  void Step(const Vector& log_input_t);

  // Retrieve the beam scorer instance used during decoding.
  CTCBeamScorer* GetBeamScorer() { return beam_scorer_.get(); }

  // Reset the beam search
  void Reset();

  // Extract the top n paths at current time step
  void TopPaths(int n, std::vector<std::vector<int>>* paths,
                std::vector<float>* log_probs, bool merge_repeated) const;

 private:
  int beam_width_;

  gtl::TopN<BeamEntry*, CTCBeamComparer> leaves_;
  std::unique_ptr<BeamEntry> beam_root_;
  std::unique_ptr<CTCBeamScorer> beam_scorer_;

  TF_DISALLOW_COPY_AND_ASSIGN(CTCBeamSearchDecoder);
};

template <typename CTCBeamState, class CTCBeamScorer, typename CTCBeamComparer>
void CTCBeamSearchDecoder<CTCBeamState, CTCBeamScorer, CTCBeamComparer>::Decode(
    const CTCDecoder::SequenceLength& seq_len,
    const std::vector<CTCDecoder::Input>& input, std::vector<CTCDecoder::Output>* output,
    ScoreOutput* scores) {
  // Storage for top paths.
  std::vector<std::vector<int>> beams;
  std::vector<float> beam_log_probabilities;
  int top_n = output->size();

  for (int b = 0; b < batch_size_; ++b) {
    int seq_len_b = seq_len[b];
    Reset();

    for (int t = 0; t < seq_len_b; ++t) {
      // Pass log-probabilities for this example + time.
      Step(input[t].row(b));
    }  // for (int t...

    // O(n * log(n))
    std::unique_ptr<std::vector<BeamEntry*>> branches(leaves_.Extract());
    leaves_.Reset();
    for (int i = 0; i < branches->size(); ++i) {
      BeamEntry* entry = (*branches)[i];
      beam_scorer_->ExpandStateEnd(&entry->state);
      entry->newp.total +=
          beam_scorer_->GetStateEndExpansionScore(entry->state);
      leaves_.push(entry);
    }

    TopPaths(top_n, &beams, &beam_log_probabilities, merge_repeated_);

    CHECK_EQ(top_n, beam_log_probabilities.size());
    CHECK_EQ(beams.size(), beam_log_probabilities.size());

    for (int i = 0; i < top_n; ++i) {
      // Copy output to the correct beam + batch
      (*output)[i][b].swap(beams[i]);
      (*scores)(b, i) = -beam_log_probabilities[i];
    }
  }  // for (int b...
}

template <typename CTCBeamState, class CTCBeamScorer, typename CTCBeamComparer>
template <typename Vector>
void CTCBeamSearchDecoder<CTCBeamState, CTCBeamScorer, CTCBeamComparer>::Step(
    const Vector& raw_input) {
  Eigen::ArrayXf input = raw_input;
  // Remove the max for stability when performing log-prob calculations.
  input -= input.maxCoeff();

  // Extract the beams sorted in decreasing new probability
  CHECK_EQ(num_classes_, input.size());

  std::unique_ptr<std::vector<BeamEntry*>> branches(leaves_.Extract());
  leaves_.Reset();

  for (BeamEntry* b : *branches) {
    // P(.. @ t) becomes the new P(.. @ t-1)
    b->oldp = b->newp;
  }

  for (BeamEntry* b : *branches) {
    if (b->parent != nullptr) {  // if not the root
      if (b->parent->Active()) {
        // If last two sequence characters are identical:
        //   Plabel(l=acc @ t=6) = (Plabel(l=acc @ t=5)
        //                          + Pblank(l=ac @ t=5))
        // else:
        //   Plabel(l=abc @ t=6) = (Plabel(l=abc @ t=5)
        //                          + P(l=ab @ t=5))
        float previous = (b->label == b->parent->label) ? b->parent->oldp.blank
                                                        : b->parent->oldp.total;
        b->newp.label =
            LogSumExp(b->newp.label,
                      beam_scorer_->GetStateExpansionScore(b->state, previous));
      }
      // Plabel(l=abc @ t=6) *= P(c @ 6)
      b->newp.label += input(b->label);
    }
    // Pblank(l=abc @ t=6) = P(l=abc @ t=5) * P(- @ 6)
    b->newp.blank = b->oldp.total + input(blank_index_);
    // P(l=abc @ t=6) = Plabel(l=abc @ t=6) + Pblank(l=abc @ t=6)
    b->newp.total = LogSumExp(b->newp.blank, b->newp.label);

    // Push the entry back to the top paths list.
    // Note, this will always fill leaves back up in sorted order.
    leaves_.push(b);
  }

  // we need to resort branches in descending oldp order.

  // branches is in descending oldp order because it was
  // originally in descending newp order and we copied newp to oldp.

  // Grow new leaves
  for (BeamEntry* b : *branches) {
    // A new leaf (represented by its BeamProbability) is a candidate
    // iff its total probability is nonzero and either the beam list
    // isn't full, or the lowest probability entry in the beam has a
    // lower probability than the leaf.
    auto is_candidate = [this](const BeamProbability& prob) {
      return (prob.total > kLogZero &&
              (leaves_.size() < beam_width_ ||
               prob.total > leaves_.peek_bottom()->newp.total));
    };

    if (!is_candidate(b->oldp)) {
      continue;
    }

    if (!b->HasChildren()) {
      b->PopulateChildren(num_classes_ - 1);
    }

    for (BeamEntry& c : *b->Children()) {
      if (!c.Active()) {
        //   Pblank(l=abcd @ t=6) = 0
        c.newp.blank = kLogZero;
        // If new child label is identical to beam label:
        //   Plabel(l=abcc @ t=6) = Pblank(l=abc @ t=5) * P(c @ 6)
        // Otherwise:
        //   Plabel(l=abcd @ t=6) = P(l=abc @ t=5) * P(d @ 6)
        beam_scorer_->ExpandState(b->state, b->label, &c.state, c.label);
        float previous = (c.label == b->label) ? b->oldp.blank : b->oldp.total;
        c.newp.label = input(c.label) +
                       beam_scorer_->GetStateExpansionScore(c.state, previous);
        // P(l=abcd @ t=6) = Plabel(l=abcd @ t=6)
        c.newp.total = c.newp.label;

        if (is_candidate(c.newp)) {
          BeamEntry* bottom = leaves_.peek_bottom();
          leaves_.push(&c);
          if (leaves_.size() == beam_width_) {
            // Bottom is no longer in the beam search.  Reset
            // its probability; signal it's no longer in the beam search.
            bottom->newp.Reset();
          }
        } else {
          // Deactivate child (signal it's not in the beam)
          c.oldp.Reset();
          c.newp.Reset();
        }
      }  // if (!c.Active()) ...
    }    // for (BeamEntry& c in children...
  }      // for (BeamEntry* b...
}

template <typename CTCBeamState, class CTCBeamScorer, typename CTCBeamComparer>
void CTCBeamSearchDecoder<CTCBeamState, CTCBeamScorer,
                          CTCBeamComparer>::Reset() {
  leaves_.Reset();

  // This beam root, and all of its children, will be in memory until
  // the next reset.
  beam_root_.reset(new BeamEntry(nullptr, -1, num_classes_ - 1, -1));
  beam_root_->newp.total = 0.0;  // ln(1)
  beam_root_->newp.blank = 0.0;  // ln(1)

  // Add the root as the initial leaf.
  leaves_.push(beam_root_.get());

  // Call initialize state on the root object.
  if (beam_scorer_) {
    beam_scorer_->InitializeState(&beam_root_->state);
  }
}

template <typename CTCBeamState, class CTCBeamScorer, typename CTCBeamComparer>
void CTCBeamSearchDecoder<CTCBeamState, CTCBeamScorer, CTCBeamComparer>::
    TopPaths(int n, std::vector<std::vector<int>>* paths,
             std::vector<float>* log_probs, bool merge_repeated) const {
  CHECK_NOTNULL(paths)->clear();
  CHECK_NOTNULL(log_probs)->clear();
  CHECK_LE(n, beam_width_) << "Requested more paths than the beam width.";
  CHECK_LE(n, leaves_.size()) << "Less leaves in the beam search "
                              << "than requested.  Have you called Step()?";

  gtl::TopN<BeamEntry*, CTCBeamComparer> top_branches(n);

  // O(beam_width_ * log(n)), space complexity is O(n)
  for (auto it = leaves_.unsorted_begin(); it != leaves_.unsorted_end(); ++it) {
    top_branches.push(*it);
  }
  // O(n * log(n))
  std::unique_ptr<std::vector<BeamEntry*>> branches(top_branches.Extract());

  for (int i = 0; i < n; ++i) {
    BeamEntry* e((*branches)[i]);
    paths->push_back(e->LabelSeq(merge_repeated));
    log_probs->push_back(e->newp.total);
  }
}

}  // namespace ctc
}  // namespace tensorflow

#endif  // TENSORFLOW_CORE_UTIL_CTC_CTC_BEAM_SEARCH_H_
