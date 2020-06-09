// Copyright (c) 2019 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "lite/operators/multiclass_nms_op.h"

#include <gtest/gtest.h>

#include <fstream>
#include <random>

#include "lite/core/op_registry.h"
#include "lite/kernels/mlu/bridges/test_helper.h"
#include "lite/kernels/npu/bridges/registry.h"

namespace paddle {
namespace lite {
namespace subgraph {
namespace mlu {

template <typename T>
void VecToFile(const std::vector<T>& vec, std::string filename) {
  std::ofstream f(filename, std::ios::out);
  if (!f) {
    LOG(FATAL) << filename << "not exist!" << std::endl;
  }
  for (size_t i = 0; i < vec.size(); i++) {
    f << vec[i] << std::endl;
  }
  f.close();
}

template <typename T>
void ArrayToFile(const T* data, int size, std::string filename) {
  std::ofstream f(filename, std::ios::out);
  if (!f) {
    LOG(FATAL) << filename << "not exist!" << std::endl;
  }
  for (size_t i = 0; i < size; i++) {
    f << data[i] << std::endl;
  }
  f.close();
}

void ToFile(Tensor* tensor, std::string file_name) {
  int count = tensor->dims().production();
  auto data = tensor->mutable_data<float>();
  std::ostringstream outs;
  for (size_t i = 0; i < count; i++) {
    outs << data[i] << std::endl;
  }
  std::ofstream of;
  of.open(file_name, std::ios::out);
  of << outs.str();
  of.close();
}

void FromFile(Tensor* tensor, std::string file_name) {
  LOG(INFO) << " from file:" << file_name << std::endl;
  std::ifstream f;
  f.open(file_name, std::ios::in);
  if (f.good()) {
    // VLOG(6) << reinterpret_cast<void*>(tensor->mutable_data<float>());
    for (size_t i = 0; i < tensor->dims().production(); i++) {
      f >> tensor->mutable_data<float>()[i];
    }
  } else {
    LOG(FATAL) << "can not open " << file_name << "to read" << std::endl;
  }
  f.close();
}

template <typename dtype>
static bool sort_score_pair_descend(const std::pair<float, dtype>& pair1,
                                    const std::pair<float, dtype>& pair2) {
  return pair1.first > pair2.first;
}

template <typename dtype>
void get_max_score_index(const dtype* scores,
                         int num,
                         float threshold,
                         int top_k,
                         std::vector<std::pair<dtype, int>>* score_index_vec) {
  // ArrayToFile(scores, 100, "cpu_score.txt");
  //! Generate index score pairs.
  for (int i = 0; i < num; ++i) {
    if (scores[i] > threshold) {
      score_index_vec->push_back(std::make_pair(scores[i], i));
    }
  }

  //! Sort the score pair according to the scores in descending order
  std::stable_sort(score_index_vec->begin(),
                   score_index_vec->end(),
                   sort_score_pair_descend<int>);

  //! Keep top_k scores if needed.
  if (top_k > -1 && top_k < score_index_vec->size()) {
    score_index_vec->resize(top_k);
  }
}

template <typename dtype>
dtype bbox_size(const dtype* bbox, bool normalized = true) {
  if (bbox[2] < bbox[0] || bbox[3] < bbox[1]) {
    // If bbox is invalid (e.g. xmax < xmin or ymax < ymin), return 0.
    return dtype(0.);
  } else {
    const dtype width = bbox[2] - bbox[0];
    const dtype height = bbox[3] - bbox[1];

    if (normalized) {
      return width * height;
    } else {
      // If bbox is not within range [0, 1].
      return (width + 1) * (height + 1);
    }
  }
}

template <typename dtype>
dtype jaccard_overlap(const dtype* bbox1, const dtype* bbox2) {
  if (bbox2[0] > bbox1[2] || bbox2[2] < bbox1[0] || bbox2[1] > bbox1[3] ||
      bbox2[3] < bbox1[1]) {
    return dtype(0.);
  } else {
    const dtype inter_xmin = std::max(bbox1[0], bbox2[0]);
    const dtype inter_ymin = std::max(bbox1[1], bbox2[1]);
    const dtype inter_xmax = std::min(bbox1[2], bbox2[2]);
    const dtype inter_ymax = std::min(bbox1[3], bbox2[3]);

    const dtype inter_width = inter_xmax - inter_xmin;
    const dtype inter_height = inter_ymax - inter_ymin;
    const dtype inter_size = inter_width * inter_height;

    const dtype bbox1_size = bbox_size(bbox1);
    const dtype bbox2_size = bbox_size(bbox2);

    return inter_size / (bbox1_size + bbox2_size - inter_size);
  }
}

template <typename dtype>
void apply_nms_fast(const dtype* bboxes,
                    const dtype* scores,
                    int num,
                    float score_threshold,
                    float nms_threshold,
                    float eta,
                    int top_k,
                    std::vector<int>* indices) {
  // Get top_k scores (with corresponding indices).
  std::vector<std::pair<dtype, int>> score_index_vec;
  get_max_score_index(scores, num, score_threshold, top_k, &score_index_vec);

  // ============= DEBUG ==========================
  // std::string filename = "score_index_vec.txt";
  // std::ofstream f(filename, std::ios::out);
  // if (!f) {
  //   LOG(FATAL) << "can not create " << filename << std::endl;
  // }
  // for (size_t i = 0; i < score_index_vec.size(); i++) {
  //   VLOG(6) << score_index_vec[i].first << " : " << score_index_vec[i].second
  //     << std::endl;
  //   // f << std::to_string(score_index_vec[i].first) << " : "
  //   //   << std::to_string([i].second) << std::endl;
  // }
  // f.close();
  // ============= DEBUG END ======================

  // Do nms.
  float adaptive_threshold = nms_threshold;
  indices->clear();

  while (score_index_vec.size() != 0) {
    const int idx = score_index_vec.front().second;
    bool keep = true;

    for (int k = 0; k < indices->size(); ++k) {
      if (keep) {
        const int kept_idx = (*indices)[k];
        float overlap =
            jaccard_overlap(bboxes + idx * 4, bboxes + kept_idx * 4);
        keep = overlap <= adaptive_threshold;
      } else {
        break;
      }
    }

    if (keep) {
      indices->push_back(idx);
    }

    score_index_vec.erase(score_index_vec.begin());

    if (keep && eta < 1 && adaptive_threshold > 0.5) {
      adaptive_threshold *= eta;
    }
  }
  // filename = "indices.txt";
  // VecToFile(*indices, filename);
  // VLOG(6) << "incices";
  // for (size_t i = 0; i < indices->size(); i++)
  // {
  //   VLOG(6) << (*indices)[i];
  // }
}

template <typename dtype>
void multiclass_nms_compute_ref(const operators::MulticlassNmsParam& param,
                                int class_num,
                                const std::vector<int>& priors,
                                bool share_location,
                                std::vector<float>* result) {
  int background_id = param.background_label;
  int keep_topk = param.keep_top_k;
  int nms_topk = param.nms_top_k;
  float conf_thresh = param.score_threshold;
  float nms_thresh = param.nms_threshold;
  float nms_eta = param.nms_eta;
  const dtype* bbox_data = param.bboxes->data<const dtype>();
  const dtype* conf_data = param.scores->data<const dtype>();
  (*result).clear();

  int num_kept = 0;
  std::vector<std::map<int, std::vector<int>>> all_indices;
  int64_t conf_offset = 0;
  int64_t bbox_offset = 0;
  for (int i = 0; i < priors.size(); ++i) {
    std::map<int, std::vector<int>> indices;
    int num_det = 0;
    int num_priors = priors[i];

    int conf_idx = class_num * conf_offset;
    int bbox_idx =
        share_location ? bbox_offset * 4 : bbox_offset * 4 * class_num;

    for (int c = 0; c < class_num; ++c) {
      if (c == background_id) {
        // Ignore background class
        continue;
      }

      const dtype* cur_conf_data = conf_data + conf_idx + c * num_priors;
      const dtype* cur_bbox_data = bbox_data + bbox_idx;

      if (!share_location) {
        cur_bbox_data += c * num_priors * 4;
      }

      apply_nms_fast(cur_bbox_data,
                     cur_conf_data,
                     num_priors,
                     conf_thresh,
                     nms_thresh,
                     nms_eta,
                     nms_topk,
                     &(indices[c]));
      num_det += indices[c].size();
      // for (size_t i = 0; i < indices[c].size(); i++)
      // {
      //   VLOG(6) << "indices[c]: " << indices[c][i];
      // }
    }

    if (keep_topk > -1 && num_det > keep_topk) {
      std::vector<std::pair<float, std::pair<int, int>>> score_index_pairs;

      for (auto it = indices.begin(); it != indices.end(); ++it) {
        int label = it->first;
        const std::vector<int>& label_indices = it->second;

        for (int j = 0; j < label_indices.size(); ++j) {
          int idx = label_indices[j];
          float score = conf_data[conf_idx + label * num_priors + idx];
          score_index_pairs.push_back(
              std::make_pair(score, std::make_pair(label, idx)));
        }
      }

      // Keep top k results per image.
      std::stable_sort(score_index_pairs.begin(),
                       score_index_pairs.end(),
                       sort_score_pair_descend<std::pair<int, int>>);
      score_index_pairs.resize(keep_topk);
      // Store the new indices.
      std::map<int, std::vector<int>> new_indices;

      for (int j = 0; j < score_index_pairs.size(); ++j) {
        int label = score_index_pairs[j].second.first;
        int idx = score_index_pairs[j].second.second;
        new_indices[label].push_back(idx);
      }

      all_indices.push_back(new_indices);
      num_kept += keep_topk;
    } else {
      all_indices.push_back(indices);
      num_kept += num_det;
    }
    conf_offset += num_priors;
    bbox_offset += num_priors;
  }

  if (num_kept == 0) {
    (*result).clear();
    (*result).resize(1);
    (*result)[0] = -1;
    return;
  } else {
    (*result).resize(num_kept * 6);
  }

  int count = 0;

  conf_offset = 0;
  bbox_offset = 0;
  for (int i = 0; i < priors.size(); ++i) {
    int num_priors = priors[i];
    int conf_idx = class_num * conf_offset;
    int bbox_idx =
        share_location ? bbox_offset * 4 : bbox_offset * 4 * class_num;

    for (auto it = all_indices[i].begin(); it != all_indices[i].end(); ++it) {
      int label = it->first;
      std::vector<int>& indices = it->second;
      const dtype* cur_conf_data = conf_data + conf_idx + label * num_priors;
      const dtype* cur_bbox_data = bbox_data + bbox_idx;

      if (!share_location) {
        cur_bbox_data += label * num_priors * 4;
      }

      for (int j = 0; j < indices.size(); ++j) {
        int idx = indices[j];
        (*result)[count * 6] = label;
        (*result)[count * 6 + 1] = cur_conf_data[idx];

        for (int k = 0; k < 4; ++k) {
          (*result)[count * 6 + 2 + k] = cur_bbox_data[idx * 4 + k];
        }

        ++count;
      }
    }
    conf_offset += num_priors;
    bbox_offset += num_priors;
  }
}

void test_multiclass_nms(float score_threshold,
                         int nms_top_k,
                         int keep_top_k,
                         float nms_threshold,
                         bool normalized,
                         float nms_eta,
                         int background_label,
                         int batch_size,
                         int class_num,
                         int num_boxes,
                         int box_size,
                         int core_num) {
  // prepare input&output variables
  Scope scope;
  std::string bboxes_var_name = "BBoxes";
  std::string scores_var_name = "Scores";
  std::string out_var_name = "Out";
  std::string out_num_var_name =
      "nms_out_num";  // must be this name,corespond with
                      // lite/operators/multiclass_nms_op.cc
  auto* bboxes = scope.Var(bboxes_var_name)->GetMutable<Tensor>();
  auto* scores = scope.Var(scores_var_name)->GetMutable<Tensor>();
  auto* out = scope.Var(out_var_name)->GetMutable<Tensor>();
  auto* out_num = scope.Var(out_num_var_name)->GetMutable<Tensor>();

  std::vector<int64_t> bboxes_shape = {batch_size, num_boxes, box_size};
  std::vector<int64_t> scores_shape = {batch_size, class_num, num_boxes};
  std::vector<int64_t> out_num_shape = {batch_size};

  bboxes->Resize(bboxes_shape);
  scores->Resize(scores_shape);
  out_num->Resize(out_num_shape);

  // initialize input&output data
  std::string bboxes_file =
      "/opt/zhaoying/projects/PaddleDetectionForDMH/paddle-dump/"
      "437_concat_2.tmp_0_1_22743_4_.txt";
  std::string scores_file =
      "/opt/zhaoying/projects/PaddleDetectionForDMH/paddle-dump/"
      "438_concat_3.tmp_0_1_80_22743_.txt";
  // FillTensor<float>(bboxes);
  // FillTensor<float>(scores);
  FromFile(bboxes, bboxes_file);
  FromFile(scores, scores_file);

  // initialize op desc
  cpp::OpDesc opdesc;
  opdesc.SetType("multiclass_nms");
  opdesc.SetInput("BBoxes", {bboxes_var_name});
  opdesc.SetInput("Scores", {scores_var_name});
  opdesc.SetOutput("Out", {out_var_name});
  opdesc.SetAttr("background_label", background_label);
  opdesc.SetAttr("keep_top_k", keep_top_k);
  opdesc.SetAttr("nms_top_k", nms_top_k);
  opdesc.SetAttr("score_threshold", score_threshold);
  opdesc.SetAttr("nms_threshold", nms_threshold);
  opdesc.SetAttr("nms_eta", nms_eta);
  opdesc.SetAttr("normalized", normalized);

  auto op = CreateOp<operators::MulticlassNmsOpLite>(opdesc, &scope);
  // out_ref->CopyDataFrom(*out);

  operators::MulticlassNmsParam param;
  auto bboxes_name = opdesc.Input("BBoxes").front();
  auto scores_name = opdesc.Input("Scores").front();
  auto out_name = opdesc.Output("Out").front();
  std::vector<std::string> output_arg_names = opdesc.OutputArgumentNames();

  param.bboxes = bboxes;
  param.scores = scores;
  param.out = out;
  param.background_label = opdesc.GetAttr<int>("background_label");
  param.keep_top_k = opdesc.GetAttr<int>("keep_top_k");
  param.nms_top_k = opdesc.GetAttr<int>("nms_top_k");
  param.score_threshold = opdesc.GetAttr<float>("score_threshold");
  param.nms_threshold = opdesc.GetAttr<float>("nms_threshold");
  param.nms_eta = opdesc.GetAttr<float>("nms_eta");
  if (opdesc.HasAttr("normalized")) {
    param.normalized = opdesc.GetAttr<bool>("normalized");
  }
  const std::vector<int>& priors = {num_boxes};  // batch_size
  std::vector<float> result;
  multiclass_nms_compute_ref<float>(param, class_num, priors, true, &result);

  // trans
  Tensor bboxes_trans;
  bboxes_trans.Resize({bboxes->dims()});
  transpose(bboxes->mutable_data<float>(),
            bboxes_trans.mutable_data<float>(),
            {static_cast<int>(bboxes->dims()[0]),
             static_cast<int>(bboxes->dims()[1]),
             static_cast<int>(bboxes->dims()[2])},
            {0, 2, 1});
  bboxes->CopyDataFrom(bboxes_trans);

  Tensor scores_trans;
  scores_trans.Resize({scores->dims()});
  transpose(scores->mutable_data<float>(),
            scores_trans.mutable_data<float>(),
            {static_cast<int>(scores->dims()[0]),
             static_cast<int>(scores->dims()[1]),
             static_cast<int>(scores->dims()[2])},
            {0, 2, 1});
  scores->CopyDataFrom(scores_trans);

  LaunchOp(
      op, {bboxes_var_name, scores_var_name}, {out_var_name, out_num_var_name});

  // ToFile(out, "nms_out_mlu_before_trans.txt");
  // out trans
  Tensor out_trans;
  out_trans.Resize({out->dims()});
  transpose(out->mutable_data<float>(),
            out_trans.mutable_data<float>(),
            {static_cast<int>(out->dims()[0]),
             static_cast<int>(out->dims()[2]),
             static_cast<int>(out->dims()[1])},  // 0 2 1 on mlu
            {0, 2, 1});
  out->CopyDataFrom(out_trans);

  ToFile(out, "nms_out_mlu.txt");
  ToFile(out_num, "nms_out_num_mlu.txt");
  VecToFile(result, "nms_out_cpu.txt");
  // compare results

  // auto out_data = out->mutable_data<float>();
  for (int i = 0; i < out->dims().production(); i++) {
    // VLOG(6) << "index: " << i;
    // nms not correspond elementwise
    // EXPECT_NEAR(out_data[i], result[i], 1e-2);
  }
}

TEST(MLUBridges, multiclass_nms) {
  int background_label = -1;
  int keep_top_k = 100;
  int nms_top_k = 1000;
  float score_threshold = 0.01;
  float nms_threshold = 0.45;
  int nms_eta = 1;
  bool normalized = 0;
  int batch_size = 1;
  int num_boxes = 22743;
  int class_num = 80;
  int core_num = 4;
  int box_size = 4;

  test_multiclass_nms(score_threshold,
                      nms_top_k,
                      keep_top_k,
                      nms_threshold,
                      normalized,
                      nms_eta,
                      background_label,
                      batch_size,
                      class_num,
                      num_boxes,
                      box_size,
                      core_num);
}

}  // namespace mlu
}  // namespace subgraph
}  // namespace lite
}  // namespace paddle

USE_SUBGRAPH_BRIDGE(multiclass_nms, kMLU)
