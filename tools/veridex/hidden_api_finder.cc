/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
  * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "hidden_api_finder.h"

#include "dex/class_accessor-inl.h"
#include "dex/code_item_accessors-inl.h"
#include "dex/dex_instruction-inl.h"
#include "dex/dex_file.h"
#include "dex/method_reference.h"
#include "hidden_api.h"
#include "resolver.h"
#include "veridex.h"

#include <iostream>

namespace art {

void HiddenApiFinder::CheckMethod(uint32_t method_id,
                                  VeridexResolver* resolver,
                                  MethodReference ref) {
  // Note: we always query whether a method is in boot, as the app
  // might define blocked APIs (which won't be used at runtime).
  const auto& name = HiddenApi::GetApiMethodName(resolver->GetDexFile(), method_id);
  method_locations_[name].push_back(ref);
}

void HiddenApiFinder::CheckField(uint32_t field_id,
                                 VeridexResolver* resolver,
                                 MethodReference ref) {
  // Note: we always query whether a field is in a boot, as the app
  // might define blocked APIs (which won't be used at runtime).
  const auto& name = HiddenApi::GetApiFieldName(resolver->GetDexFile(), field_id);
  field_locations_[name].push_back(ref);
}

void HiddenApiFinder::CollectAccesses(VeridexResolver* resolver,
                                      const ClassFilter& class_filter) {
  const DexFile& dex_file = resolver->GetDexFile();
  // Look at all types referenced in this dex file. Any of these
  // types can lead to being used through reflection.
  for (uint32_t i = 0; i < dex_file.NumTypeIds(); ++i) {
    std::string name(dex_file.GetTypeDescriptorView(dex::TypeIndex(i)));
    classes_.insert(name);
  }
  // Note: we collect strings constants only referenced in code items as the string table
  // contains other kind of strings (eg types).
  for (ClassAccessor accessor : dex_file.GetClasses()) {
    if (class_filter.Matches(accessor.GetDescriptor())) {
      for (const ClassAccessor::Method& method : accessor.GetMethods()) {
        CodeItemInstructionAccessor codes = method.GetInstructions();
        const uint32_t max_pc = codes.InsnsSizeInCodeUnits();
        for (const DexInstructionPcPair& inst : codes) {
          if (inst.DexPc() >= max_pc) {
            // We need to prevent abnormal access for outside of code
            break;
          }

          switch (inst->Opcode()) {
            case Instruction::CONST_STRING: {
              dex::StringIndex string_index(inst->VRegB_21c());
              const std::string name(dex_file.GetStringView(string_index));
              // Cheap filtering on the string literal. We know it cannot be a field/method/class
              // if it contains a space.
              if (name.find(' ') == std::string::npos) {
                // Class names at the Java level are of the form x.y.z, but the list encodes
                // them of the form Lx/y/z;. Inner classes have '$' for both Java level class
                // names in strings, and hidden API lists.
                std::string str = HiddenApi::ToInternalName(name);
                // Note: we can query the lists directly, as HiddenApi added classes that own
                // private methods and fields in them.
                // We don't add class names to the `strings_` set as we know method/field names
                // don't have '.' or '/'. All hidden API class names have a '/'.
                if (hidden_api_.IsInBoot(str)) {
                  classes_.insert(str);
                } else if (hidden_api_.IsInBoot(name)) {
                  // Could be something passed to JNI.
                  classes_.insert(name);
                } else {
                  // We only keep track of the location for strings, as these will be the
                  // field/method names the user is interested in.
                  strings_.insert(name);
                  reflection_locations_[name].push_back(method.GetReference());
                }
              }
              break;
            }
            case Instruction::INVOKE_DIRECT:
            case Instruction::INVOKE_INTERFACE:
            case Instruction::INVOKE_STATIC:
            case Instruction::INVOKE_SUPER:
            case Instruction::INVOKE_VIRTUAL: {
              CheckMethod(inst->VRegB_35c(), resolver, method.GetReference());
              break;
            }

            case Instruction::INVOKE_DIRECT_RANGE:
            case Instruction::INVOKE_INTERFACE_RANGE:
            case Instruction::INVOKE_STATIC_RANGE:
            case Instruction::INVOKE_SUPER_RANGE:
            case Instruction::INVOKE_VIRTUAL_RANGE: {
              CheckMethod(inst->VRegB_3rc(), resolver, method.GetReference());
              break;
            }

            case Instruction::IGET:
            case Instruction::IGET_WIDE:
            case Instruction::IGET_OBJECT:
            case Instruction::IGET_BOOLEAN:
            case Instruction::IGET_BYTE:
            case Instruction::IGET_CHAR:
            case Instruction::IGET_SHORT: {
              CheckField(inst->VRegC_22c(), resolver, method.GetReference());
              break;
            }

            case Instruction::IPUT:
            case Instruction::IPUT_WIDE:
            case Instruction::IPUT_OBJECT:
            case Instruction::IPUT_BOOLEAN:
            case Instruction::IPUT_BYTE:
            case Instruction::IPUT_CHAR:
            case Instruction::IPUT_SHORT: {
              CheckField(inst->VRegC_22c(), resolver, method.GetReference());
              break;
            }

            case Instruction::SGET:
            case Instruction::SGET_WIDE:
            case Instruction::SGET_OBJECT:
            case Instruction::SGET_BOOLEAN:
            case Instruction::SGET_BYTE:
            case Instruction::SGET_CHAR:
            case Instruction::SGET_SHORT: {
              CheckField(inst->VRegB_21c(), resolver, method.GetReference());
              break;
            }

            case Instruction::SPUT:
            case Instruction::SPUT_WIDE:
            case Instruction::SPUT_OBJECT:
            case Instruction::SPUT_BOOLEAN:
            case Instruction::SPUT_BYTE:
            case Instruction::SPUT_CHAR:
            case Instruction::SPUT_SHORT: {
              CheckField(inst->VRegB_21c(), resolver, method.GetReference());
              break;
            }

            default:
              break;
          }
        }
      }
    }
  }
}

void HiddenApiFinder::Run(const std::vector<std::unique_ptr<VeridexResolver>>& resolvers,
                          const ClassFilter& class_filter) {
  for (const std::unique_ptr<VeridexResolver>& resolver : resolvers) {
    CollectAccesses(resolver.get(), class_filter);
  }
}

void HiddenApiFinder::Dump(std::ostream& os,
                           HiddenApiStats* stats,
                           bool dump_reflection) {
  // Dump methods from hidden APIs linked against.
  for (const std::pair<const std::string,
                       std::vector<MethodReference>>& pair : method_locations_) {
    const auto& name = pair.first;
    if (hidden_api_.GetSignatureSource(name) != SignatureSource::APP &&
        hidden_api_.ShouldReport(name)) {
      stats->linking_count++;
      hiddenapi::ApiList api_list = hidden_api_.GetApiList(pair.first);
      stats->api_counts[api_list.GetIntValue()]++;
      os << "#" << ++stats->count << ": Linking " << api_list << " " << pair.first << " use(s):";
      os << std::endl;
      HiddenApiFinder::DumpReferences(os, pair.second);
      os << std::endl;
    }
  }

  // Dump fields from hidden APIs linked against.
  for (const std::pair<const std::string,
                       std::vector<MethodReference>>& pair : field_locations_) {
    const auto& name = pair.first;
    if (hidden_api_.GetSignatureSource(name) != SignatureSource::APP &&
        hidden_api_.ShouldReport(name)) {
      stats->linking_count++;
      hiddenapi::ApiList api_list = hidden_api_.GetApiList(pair.first);
      stats->api_counts[api_list.GetIntValue()]++;
      // Note: There is a test depending on this output format,
      // so please be careful when you modify the format. b/123662832
      os << "#" << ++stats->count << ": Linking " << api_list << " " << pair.first << " use(s):";
      os << std::endl;
      HiddenApiFinder::DumpReferences(os, pair.second);
      os << std::endl;
    }
  }

  if (dump_reflection) {
    // Dump potential reflection uses.
    for (const std::string& cls : classes_) {
      for (const std::string& name : strings_) {
        std::string full_name = ART_FORMAT("{}->{}", cls, name);
        if (hidden_api_.GetSignatureSource(full_name) != SignatureSource::APP &&
            hidden_api_.ShouldReport(full_name)) {
          hiddenapi::ApiList api_list = hidden_api_.GetApiList(full_name);
          stats->api_counts[api_list.GetIntValue()]++;
          stats->reflection_count++;
          // Note: There is a test depending on this output format,
          // so please be careful when you modify the format. b/123662832
          os << "#" << ++stats->count << ": Reflection " << api_list << " " << full_name
             << " potential use(s):";
          os << std::endl;
          HiddenApiFinder::DumpReferences(os, reflection_locations_[name]);
          os << std::endl;
        }
      }
    }
  }
}

void HiddenApiFinder::DumpReferences(std::ostream& os,
                                     const std::vector<MethodReference>& references) {
  static const char* kPrefix = "       ";

  // Count number of occurrences of each reference, to make the output clearer.
  std::map<std::string, size_t> counts;
  for (const MethodReference& ref : references) {
    std::string ref_string = HiddenApi::GetApiMethodName(ref);
    if (!counts.count(ref_string)) {
      counts[ref_string] = 0;
    }
    counts[ref_string]++;
  }

  for (const std::pair<const std::string, size_t>& pair : counts) {
    os << kPrefix << pair.first;
    if (pair.second > 1) {
       os << " (" << pair.second << " occurrences)";
    }
    os << std::endl;
  }
}

}  // namespace art
