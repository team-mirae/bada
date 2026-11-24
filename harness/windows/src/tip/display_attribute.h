// Adapted from bada-windows src/tip/display_attribute.* (jis-moonshot's).
// Display attributes: how apps underline the marked text, by the kind of its segment.
#pragma once

#include <msctf.h>

namespace bada::tip {

IEnumTfDisplayAttributeInfo* NewDisplayAttributeEnum();
ITfDisplayAttributeInfo* NewDisplayAttributeInfo(REFGUID guid);  // nullptr if not ours

}  // namespace bada::tip
