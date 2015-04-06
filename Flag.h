#ifndef AAPT_FLAG_H
#define AAPT_FLAG_H

#include "StringPiece.h"

#include <functional>
#include <string>
#include <vector>

namespace aapt {
namespace flag {

void requiredFlag(const StringPiece& name, const StringPiece& description,
                  std::function<void(const StringPiece&)> action);

void optionalFlag(const StringPiece& name, const StringPiece& description,
                  std::function<void(const StringPiece&)> action);

void optionalSwitch(const StringPiece& name, const StringPiece& description, bool* result);

void parse(int argc, char** argv, const StringPiece& command);

const std::vector<std::string>& getArgs();

} // namespace flag
} // namespace aapt

#endif // AAPT_FLAG_H
