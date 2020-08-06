/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

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

#include <jni.h>

#include <memory>
#include <string>

#include "absl/strings/str_cat.h"
#include "tensorflow_lite_support/cc/port/statusor.h"
#include "tensorflow_lite_support/cc/task/vision/core/frame_buffer.h"
#include "tensorflow_lite_support/cc/task/vision/image_segmenter.h"
#include "tensorflow_lite_support/cc/task/vision/proto/image_segmenter_options_proto_inc.h"
#include "tensorflow_lite_support/cc/task/vision/proto/segmentations_proto_inc.h"
#include "tensorflow_lite_support/cc/task/vision/utils/frame_buffer_common_utils.h"
#include "tensorflow_lite_support/cc/utils/jni_utils.h"
#include "tensorflow_lite_support/java/src/native/task/vision/jni_utils.h"

namespace {

using ::tflite::support::StatusOr;
using ::tflite::support::task::vision::ConvertToFrameBufferOrientation;
using ::tflite::support::task::vision::FrameBuffer;
using ::tflite::support::task::vision::ImageSegmenter;
using ::tflite::support::task::vision::ImageSegmenterOptions;
using ::tflite::support::task::vision::Segmentation;
using ::tflite::support::task::vision::SegmentationResult;
using ::tflite::support::utils::CreateByteArray;
using ::tflite::support::utils::GetMappedFileBuffer;
using ::tflite::support::utils::kAssertionError;
using ::tflite::support::utils::kIllegalArgumentException;
using ::tflite::support::utils::kInvalidPointer;
using ::tflite::support::utils::ThrowException;

constexpr char kArrayListClassName[] = "Ljava/util/ArrayList;";
constexpr char kObjectClassName[] = "Ljava/lang/Object;";
constexpr char kColorClassName[] = "Landroid/graphics/Color;";
constexpr char kColoredLabelClassName[] =
    "Lorg/tensorflow/lite/task/vision/segmenter/ColoredLabel;";
constexpr char kStringClassName[] = "Ljava/lang/String;";
constexpr int kOutputTypeCategoryMask = 0;
constexpr int kOutputTypeConfidenceMask = 1;

// Creates an ImageSegmenterOptions proto based on the Java class.
ImageSegmenterOptions ConvertToProtoOptions(JNIEnv* env,
                                            jstring display_names_locale,
                                            jint output_type) {
  ImageSegmenterOptions proto_options;

  const char* pchars = env->GetStringUTFChars(display_names_locale, nullptr);
  proto_options.set_display_names_locale(pchars);
  env->ReleaseStringUTFChars(display_names_locale, pchars);

  switch (output_type) {
    case kOutputTypeCategoryMask:
      proto_options.set_output_type(ImageSegmenterOptions::CATEGORY_MASK);
      break;
    case kOutputTypeConfidenceMask:
      proto_options.set_output_type(ImageSegmenterOptions::CONFIDENCE_MASK);
      break;
    default:
      // Should never happen.
      ThrowException(env, kIllegalArgumentException,
                     "Unsupported output type: %d", output_type);
  }

  return proto_options;
}

void ConvertToSegmentationResults(JNIEnv* env,
                                  const SegmentationResult& results,
                                  jobject jmask_buffers, jintArray jmask_shape,
                                  jobject jcolored_labels) {
  if (results.segmentation_size() != 1) {
    // Should never happen.
    ThrowException(
        env, kAssertionError,
        "ImageSegmenter only supports one segmentation result, getting %d",
        results.segmentation_size());
  }

  const Segmentation& segmentation = results.segmentation(0);

  // Get the shape from the C++ Segmentation results.
  int shape_array[2] = {segmentation.height(), segmentation.width()};
  env->SetIntArrayRegion(jmask_shape, 0, 2, shape_array);

  // jclass, init, and add of ArrayList.
  jclass array_list_class = env->FindClass(kArrayListClassName);
  jmethodID array_list_add_method =
      env->GetMethodID(array_list_class, "add",
                       absl::StrCat("(", kObjectClassName, ")Z").c_str());

  // Convert the masks into ByteBuffer list.
  int num_pixels = segmentation.height() * segmentation.width();
  if (segmentation.has_category_mask()) {
    jbyteArray byte_array = CreateByteArray(
        env,
        reinterpret_cast<const jbyte*>(segmentation.category_mask().data()),
        num_pixels * sizeof(uint8));
    env->CallBooleanMethod(jmask_buffers, array_list_add_method, byte_array);
    env->DeleteLocalRef(byte_array);
  } else {
    for (const auto& confidence_mask :
         segmentation.confidence_masks().confidence_mask()) {
      jbyteArray byte_array = CreateByteArray(
          env, reinterpret_cast<const jbyte*>(confidence_mask.value().data()),
          num_pixels * sizeof(float));
      env->CallBooleanMethod(jmask_buffers, array_list_add_method, byte_array);
      env->DeleteLocalRef(byte_array);
    }
  }

  // Convert colored labels from the C++ object to the Java object.
  jclass color_class = env->FindClass(kColorClassName);
  jmethodID color_value_of_method = env->GetStaticMethodID(
      color_class, "valueOf", absl::StrCat("(I)", kColorClassName).c_str());
  jmethodID color_rgb_method =
      env->GetStaticMethodID(color_class, "rgb", "(III)I");
  jclass colored_label_class = env->FindClass(kColoredLabelClassName);
  jmethodID colored_label_create_method = env->GetStaticMethodID(
      colored_label_class, "create",
      absl::StrCat("(", kStringClassName, kStringClassName, kColorClassName,
                   ")", kColoredLabelClassName)
          .c_str());

  for (const auto& colored_label : segmentation.colored_labels()) {
    jstring label = env->NewStringUTF(colored_label.class_name().c_str());
    jstring display_name =
        env->NewStringUTF(colored_label.display_name().c_str());
    jint rgb =
        env->CallIntMethod(color_class, color_rgb_method, colored_label.r(),
                           colored_label.g(), colored_label.b());
    jobject color =
        env->CallStaticObjectMethod(color_class, color_value_of_method, rgb);

    jobject jcolored_label = env->CallStaticObjectMethod(
        colored_label_class, colored_label_create_method, label, display_name,
        color);
    env->CallBooleanMethod(jcolored_labels, array_list_add_method,
                           jcolored_label);

    env->DeleteLocalRef(label);
    env->DeleteLocalRef(display_name);
    env->DeleteLocalRef(color);
    env->DeleteLocalRef(jcolored_label);
  }
}

extern "C" JNIEXPORT void JNICALL
Java_org_tensorflow_lite_task_core_BaseTaskApi_deinitJni(JNIEnv* env,
                                                         jobject thiz,
                                                         jlong native_handle) {
  delete reinterpret_cast<ImageSegmenter*>(native_handle);
}

extern "C" JNIEXPORT jlong JNICALL
Java_org_tensorflow_lite_task_vision_segmenter_ImageSegmenter_initJniWithModelFdAndOptions(
    JNIEnv* env, jclass thiz, jint file_descriptor,
    jlong file_descriptor_length, jlong file_descriptor_offset,
    jstring display_names_locale, jint output_type) {
  ImageSegmenterOptions proto_options =
      ConvertToProtoOptions(env, display_names_locale, output_type);
  auto file_descriptor_meta = proto_options.mutable_model_file_with_metadata()
                                  ->mutable_file_descriptor_meta();
  file_descriptor_meta->set_fd(file_descriptor);
  file_descriptor_meta->set_length(file_descriptor_length);
  file_descriptor_meta->set_offset(file_descriptor_offset);

  StatusOr<std::unique_ptr<ImageSegmenter>> image_segmenter_or =
      ImageSegmenter::CreateFromOptions(proto_options);
  if (image_segmenter_or.ok()) {
    return reinterpret_cast<jlong>(image_segmenter_or->release());
  } else {
    ThrowException(env, kAssertionError,
                   "Error occurred when initializing ImageSegmenter: %s",
                   image_segmenter_or.status().message().data());
    return kInvalidPointer;
  }
}

extern "C" JNIEXPORT void JNICALL
Java_org_tensorflow_lite_task_vision_segmenter_ImageSegmenter_segmentNative(
    JNIEnv* env, jclass thiz, jlong native_handle, jobject jimage_byte_buffer,
    jint width, jint height, jobject jmask_buffers, jintArray jmask_shape,
    jobject jcolored_labels, jint jorientation) {
  auto* segmenter = reinterpret_cast<ImageSegmenter*>(native_handle);
  absl::string_view image = GetMappedFileBuffer(env, jimage_byte_buffer);
  std::unique_ptr<FrameBuffer> frame_buffer = CreateFromRgbRawBuffer(
      reinterpret_cast<const uint8*>(image.data()),
      FrameBuffer::Dimension{width, height},
      ConvertToFrameBufferOrientation(env, jorientation));
  auto results_or = segmenter->Segment(*frame_buffer);
  if (results_or.ok()) {
    ConvertToSegmentationResults(env, results_or.value(), jmask_buffers,
                                 jmask_shape, jcolored_labels);
  } else {
    ThrowException(env, kAssertionError,
                   "Error occurred when segmenting the image: %s",
                   results_or.status().message().data());
  }
}

}  // namespace