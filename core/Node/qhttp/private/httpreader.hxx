/** @file httpreader.hxx
 *
 * @copyright (C) 2016
 * @date 2016.05.26
 * @version 1.0.0
 * @author amir zamani <azadkuh@live.com>
 *
 */

#ifndef __QHTTP_HTTPREADER_HXX__
#define __QHTTP_HTTPREADER_HXX__

#include "qhttpbase.hpp"
///////////////////////////////////////////////////////////////////////////////
namespace qhttp {
namespace details {
///////////////////////////////////////////////////////////////////////////////

// usage in client::QHttpResponse, server::QHttpRequest
template<class TBase>
class HttpReader : public TBase
{
public:
    enum TReadState {
        EEmpty,
        EPartial,
        EComplete,
        ESent
    };

public:
    void collectData(int atMost) {
        icollectRequired = true;
        icollectCapacity = atMost;
        if ( atMost > 0 )
            icollectedData.reserve(atMost);
    }

    void stopCollectData() {
        icollectRequired = false;
        icollectCapacity = 0;
        icollectedData.clear();
    }

    QByteArray getCollectedData() {
        icollectRequired = false;
        icollectCapacity = 0;
        return icollectedData;
    }



    bool append(const char* data, size_t length) {
        if ( !icollectRequired ) // not allowed to collect data
            return false;

        icollectedData.append(data, length);

        return true;
    }

    // call cb if the message is not finalized yet
    template<class Func>
    void finalizeSending(Func cb) {
        if ( ireadState != EComplete ) {
            ireadState  = EComplete;
            isuccessful = true;

            cb();
        }
    }

public:

    bool       isuccessful      = false;
    TReadState ireadState       = EEmpty;

    /// shall I collect incoming body data by myself?
    bool       icollectRequired = false;
    int        icollectCapacity = 0;
    QByteArray icollectedData;
};

///////////////////////////////////////////////////////////////////////////////
} // namespace details
} // namespace qhttp
///////////////////////////////////////////////////////////////////////////////
#endif // __QHTTP_HTTPREADER_HXX__
