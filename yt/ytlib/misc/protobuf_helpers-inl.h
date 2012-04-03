#ifndef PROTOBUF_HELPERS_INL_H_
#error "Direct inclusion of this file is not allowed, include protobuf_helpers.h"
#endif
#undef PROTOBUF_HELPERS_INL_H_

#include "foreach.h"
#include "assert.h"

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

template <class T>
TAutoPtr<T> GetProtoExtension(const NProto::TExtensionSet& extensions)
{
    auto result = FindProtoExtension<T>(extensions);
    YASSERT(result);
    return result;
}

template <class T>
TAutoPtr<T> FindProtoExtension(const NProto::TExtensionSet& extensions)
{
    i32 tag = GetProtoExtensionTag<T>();
    FOREACH (const auto& extension, extensions.extensions()) {
        if (extension.tag() == tag) {
            const auto& data = extension.data();
            TAutoPtr<T> result(new T());
            YVERIFY(result->ParseFromArray(data.begin(), data.length()));
            return result;
        }
    }
    return NULL;
}

template <class T>
void SetProtoExtension(NProto::TExtensionSet* extensions, const T& value)
{
    i32 tag = GetProtoExtensionTag<T>();
    NProto::TExtension* extension = NULL;
    FOREACH (const auto& currentExtension, extensions->extensions()) {
        if (currentExtension.tag() == tag) {
            extension = &currentExtension;
            break;
        }
    }
    if (!extension) {
        extension = extensions->add_extensions();
    }

    int size = extension.ByteSize();
    Stroka str(size);
    YVERIFY(value.SerializeToArray(str.begin(), size));
    extension->set_data(str);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
