/*
 * Copyright (C) 2016 The Android Open Source Project
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

#include "xml/XmlActionExecutor.h"

namespace aapt {
namespace xml {

static bool wrapper_one(XmlNodeAction::ActionFunc& f, Element* el,
                        SourcePathDiagnostics*) {
  return f(el);
}

static bool wrapper_two(XmlNodeAction::ActionFuncWithDiag& f, Element* el,
                        SourcePathDiagnostics* diag) {
  return f(el, diag);
}

void XmlNodeAction::Action(XmlNodeAction::ActionFunc f) {
  actions_.emplace_back(std::bind(
      wrapper_one, std::move(f), std::placeholders::_1, std::placeholders::_2));
}

void XmlNodeAction::Action(XmlNodeAction::ActionFuncWithDiag f) {
  actions_.emplace_back(std::bind(
      wrapper_two, std::move(f), std::placeholders::_1, std::placeholders::_2));
}

static void PrintElementToDiagMessage(const Element* el, DiagMessage* msg) {
  *msg << "<";
  if (!el->namespace_uri.empty()) {
    *msg << el->namespace_uri << ":";
  }
  *msg << el->name << ">";
}

bool XmlNodeAction::Execute(XmlActionExecutorPolicy policy,
                            SourcePathDiagnostics* diag, Element* el) const {
  bool error = false;
  for (const ActionFuncWithDiag& action : actions_) {
    error |= !action(el, diag);
  }

  for (Element* child_el : el->GetChildElements()) {
    if (child_el->namespace_uri.empty()) {
      std::map<std::string, XmlNodeAction>::const_iterator iter =
          map_.find(child_el->name);
      if (iter != map_.end()) {
        error |= !iter->second.Execute(policy, diag, child_el);
        continue;
      }
    }

    if (policy == XmlActionExecutorPolicy::kWhitelist) {
      DiagMessage error_msg(child_el->line_number);
      error_msg << "unknown element ";
      PrintElementToDiagMessage(child_el, &error_msg);
      error_msg << " found";
      diag->Error(error_msg);
      error = true;
    }
  }
  return !error;
}

bool XmlActionExecutor::Execute(XmlActionExecutorPolicy policy,
                                IDiagnostics* diag, XmlResource* doc) const {
  SourcePathDiagnostics source_diag(doc->file.source, diag);

  Element* el = FindRootElement(doc);
  if (!el) {
    if (policy == XmlActionExecutorPolicy::kWhitelist) {
      source_diag.Error(DiagMessage() << "no root XML tag found");
      return false;
    }
    return true;
  }

  if (el->namespace_uri.empty()) {
    std::map<std::string, XmlNodeAction>::const_iterator iter =
        map_.find(el->name);
    if (iter != map_.end()) {
      return iter->second.Execute(policy, &source_diag, el);
    }
  }

  if (policy == XmlActionExecutorPolicy::kWhitelist) {
    DiagMessage error_msg(el->line_number);
    error_msg << "unknown element ";
    PrintElementToDiagMessage(el, &error_msg);
    error_msg << " found";
    source_diag.Error(error_msg);
    return false;
  }
  return true;
}

}  // namespace xml
}  // namespace aapt
