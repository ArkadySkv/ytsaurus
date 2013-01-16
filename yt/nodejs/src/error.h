#pragma once

#include "common.h"

#include <ytlib/misc/error.h>

namespace NYT {
namespace NNodeJS {

////////////////////////////////////////////////////////////////////////////////

v8::Handle<v8::Value> ConvertErrorToV8(const NYT::TError& error);

void InitializeError(v8::Handle<v8::Object> target);

////////////////////////////////////////////////////////////////////////////////

} // namespace NNodeJS
} // namespace NYT

