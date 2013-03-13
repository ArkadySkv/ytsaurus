#pragma once

#include "public.h"
#include <ytlib/yson/yson_consumer.h>

#include <ytlib/misc/error.h>

namespace NYT {
namespace NYTree {

////////////////////////////////////////////////////////////////////////////////

struct ISystemAttributeProvider
{
    virtual ~ISystemAttributeProvider()
    { }

    //! Describes a system attribute.
    struct TAttributeInfo
    {
        const char* Key;
        bool IsPresent;
        bool IsOpaque;

        TAttributeInfo(const char* key, bool isPresent = true, bool isOpaque = false)
            : Key(key)
            , IsPresent(isPresent)
            , IsOpaque(isOpaque)
        { }
    };

    //! Populates the list of all system attributes supported by this object.
    /*!
     *  \note
     *  Must not clear #attributes since additional items may be added in inheritors.
     */
    virtual void ListSystemAttributes(std::vector<TAttributeInfo>* attributes) = 0;

    //! Gets the value of a system attribute.
    /*!
     *  \returns False if there is no system attribute with the given key.
     */
    virtual bool GetSystemAttribute(const Stroka& key, NYson::IYsonConsumer* consumer) = 0;

    //! Asynchronously gets the value of a system attribute.
    /*!
     *  \returns Null if there is no such async system attribute with the given key.
     */
    virtual TAsyncError GetSystemAttributeAsync(const Stroka& key, NYson::IYsonConsumer* consumer) = 0;

    //! Sets the value of a system attribute.
    /*!
     *  \returns False if there is no writable system attribute with the given key.
     */
    virtual bool SetSystemAttribute(const Stroka& key, const TYsonString& value) = 0;


    // Extension methods.

    //! Returns an instance of TAttributeInfo matching a given #key or |NULL| if no such
    //! system attribute is known.
    TAttributeInfo* FindSystemAttributeInfo(const Stroka& key);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT
