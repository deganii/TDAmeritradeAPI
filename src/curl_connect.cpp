/*
Copyright (C) 2018 Jonathon Ogden <jeog.dev@gmail.com>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see http://www.gnu.org/licenses.
*/

#include <sstream>
#include <unordered_map>
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <regex>

#include <assert.h>

#include "../include/curl_connect.h"
//#include "../include/util.h"

using std::string;
using std::map;
using std::vector;
using std::pair;
using std::tuple;
using std::ostream;


namespace conn{

class CurlConnection::CurlConnectionImpl_ { 
    static struct Init {
        Init() { curl_global_init(CURL_GLOBAL_ALL); }
        /*
        * IMPORTANT
        *
        * need to be sure we don't cleanup libcurl before accessing streamer
        * or it creates issues when uWebSockets tries to create an SSL context;
        * EVP_get_digestbyname("ssl2-md5") returns null which seg faults in
        * SSL_CTX_ctrl in versions < 1.1.0
        */
        ~Init() { curl_global_cleanup(); }
    }_init;

    struct curl_slist *_header;
    CURL *_handle;
    map<CURLoption, string> _options;
    char *_error_buffer;
 
    /* to string overloads for our different stored option values */
    template<typename T, typename Dummy = void>
    struct to;

    struct WriteCallback {
        std::stringbuf _buf;

        static size_t
        write( char* input, size_t sz, size_t n, void* output )
        {
            std::stringbuf& buf = ((WriteCallback*)output)->_buf;
            std::streamsize ssz = buf.sputn(input, sz*n);
            //assert(ssz >= 0);
            return ssz;
        }

        string
        str()
        { return _buf.str(); }

        void
        clear()
        { _buf.str(""); }
    };

public:
    CurlConnectionImpl_(string url)
        :
            CurlConnectionImpl_()
        {
            SET_url(url);
        }
    
    CurlConnectionImpl_()
        :
            _header(nullptr),
            _handle(curl_easy_init()),
            _error_buffer(new char[CURL_ERROR_SIZE+1])
        {
            set_option(CURLOPT_NOSIGNAL, 1L);
            _error_buffer[CURL_ERROR_SIZE] = 0;
            set_option(CURLOPT_ERRORBUFFER, _error_buffer);
        }
    
    ~CurlConnectionImpl_()
        {
            close();
            delete[] _error_buffer;
        }


    CurlConnectionImpl_(CurlConnectionImpl_&& connection)
        :
            _header(connection._header),
            _handle(connection._handle),
            _options(move(connection._options)),
            _error_buffer(connection._error_buffer)
        {
            connection._header = nullptr;
            connection._handle = nullptr;
            connection._error_buffer = nullptr;
        }
    
    CurlConnectionImpl_&
    operator=(CurlConnectionImpl_&& connection)
    {
        if (*this != connection) {
            if (_header)
                curl_slist_free_all(_header);

            if (_handle)
                curl_easy_cleanup(_handle);

            if (_error_buffer)
                delete[] _error_buffer;

            _header = connection._header;
            _handle = connection._handle;
            _options = move(connection._options);
            _error_buffer = connection._error_buffer;

            connection._header = nullptr;
            connection._handle = nullptr;
            connection._error_buffer = nullptr;
        }
        return *this;
    }

    CurlConnectionImpl_(const CurlConnectionImpl_& connection) = delete;

    CurlConnectionImpl_&
    operator=( const CurlConnectionImpl_& connection) = delete;


    bool /* only use is to avoid move-assign to self */
    operator==(const CurlConnectionImpl_& connection)
    {
        return (_handle == connection._handle &&
                _header == connection._header &&
                _options == connection._options &&
                _error_buffer == connection._error_buffer );
    }

    bool
    operator!=(const CurlConnectionImpl_& connection)
    {
        return !operator==(connection);
    }

    const map<CURLoption, string>&
    get_option_strings() const
    { return _options; }

    template<typename T>
    void
    set_option(CURLoption option, T param)
    {
        static_assert(!std::is_same<T, string>::value,
            "CurlConnection::set_option doesn't accept string");
        if (!_handle) {
            throw CurlException("connection/handle has been closed");
        }
        if (curl_easy_setopt(_handle, option, param) != CURLE_OK) {
            throw CurlOptionException(option, to<T>::str(param));
        }
        /*
        * NOTE we don't do anything special with function pointers
        *      (e.g CURLOPT_WRITEFUNCTION) or list poiners
        *      (e.g CURLOPT_HTTPHEADER) except store the address.
        *      See 'to' struct specializations.
        */
        _options[option] = to<T>::str(param);
    }
    
    // <status code, data, header, time>
    tuple<long, string, string, clock_ty::time_point>
    execute( bool return_header_data )
    {
        if (!_handle)
            throw CurlException("connection/handle has been closed");

        WriteCallback cb_data, cb_header;
        set_option(CURLOPT_WRITEFUNCTION, &WriteCallback::write);
        set_option(CURLOPT_WRITEDATA, &cb_data);

        if( return_header_data ){
            set_option(CURLOPT_HEADERFUNCTION, &WriteCallback::write);
            set_option(CURLOPT_HEADERDATA, &cb_header);
        }

        CURLcode ccode = curl_easy_perform(_handle);
        auto tp = clock_ty::now();
        if (ccode != CURLE_OK)
            throw CurlConnectionError(ccode, _error_buffer);

        string res = cb_data.str();
        cb_data.clear();
        long c;
        curl_easy_getinfo(_handle, CURLINFO_RESPONSE_CODE, &c);

        string head;
        if( return_header_data ){
            head = cb_header.str();
            cb_data.clear();
        }

        return make_tuple(c, res, head, tp);
    }

    void
    close()
    {
        if (_header) {
            curl_slist_free_all(_header);
            _header = nullptr;
        }
        if (_handle) {
            curl_easy_cleanup(_handle);
            _handle = nullptr;
        }
        _options.clear();
    }

    bool
    is_closed() const
    { return _handle == nullptr; }
    
    operator bool() const
    { return !is_closed(); }
    
    void
    SET_url(string url)
    { set_option(CURLOPT_URL, url.c_str()); 
		
		    //    if (curl_easy_setopt(_handle, CURLSSLOPT_NO_REVOKE, 1L) != CURLE_OK) {
        //    throw CurlOptionException(option, to<T>::str(param));
        //}
				
		
			 set_option(CURLOPT_SSL_OPTIONS, CURLSSLOPT_NO_REVOKE); 
		}

    string
    GET_url() const
    {
        auto v = _options.find(CURLOPT_URL);
        return (v == _options.end()) ? "" : v->second;
    }

    void
    SET_ssl_verify(bool on)
    {
        set_option(CURLOPT_SSL_VERIFYPEER, on ? 1L : 0L);
        set_option(CURLOPT_SSL_VERIFYHOST, on ? 2L : 0L);
    }

    void
    SET_ssl_verify_using_ca_bundle(string path)
    {
        SET_ssl_verify(true);
        set_option(CURLOPT_CAINFO, path.c_str());
    }

    void
    SET_ssl_verify_using_ca_certs(string dir)
    {
        SET_ssl_verify(true);
        set_option(CURLOPT_CAPATH, dir.c_str());
    }

    void
    SET_encoding(string enc)
    { set_option(CURLOPT_ACCEPT_ENCODING, enc.c_str()); }
    
    void
    SET_keepalive(bool on)
    { set_option(CURLOPT_TCP_KEEPALIVE, on ? 1L : 0L); }

    void
    SET_timeout(long timeout)
    { set_option(CURLOPT_TIMEOUT_MS, (timeout > 0 ? timeout : 0)); }

    long
    GET_timeout() const
    {
        auto f = _options.find(CURLOPT_TIMEOUT_MS);
        return (f != _options.end()) ? std::stol(f->second) : 0;
    }

    void
    ADD_headers(const vector<pair<string, string>>& headers)
    {
        if (!_handle)
            throw CurlException("connection/handle has been closed");

        if (headers.empty())
            return;

        for (auto& h : headers) {
            string s = h.first + ": " + h.second;
            _header = curl_slist_append(_header, s.c_str());
            if (!_header) {
                throw CurlOptionException("curl_slist_append failed trying to "
                    "add header", CURLOPT_HTTPHEADER, s);
            }
        }

        return set_option(CURLOPT_HTTPHEADER, _header);
    }
    
    vector<pair<string,string>>
    GET_headers() const
    {
        struct curl_slist * tmp = _header;

        vector<pair<string,string>> headers;
        while( tmp ){
             string h(tmp->data);
             size_t pos = h.find(": ");
             if( pos == string::npos )
                 throw CurlException("malformed header");
             headers.emplace_back( string(h.begin(), h.begin() + pos),
                                   string(h.begin() + pos + 2, h.end()) );
             tmp = tmp->next;
        }
        return headers;
    }

    void
    RESET_headers()
    {
        curl_slist_free_all(_header);
        _header = nullptr;
        _options.erase(CURLOPT_HTTPHEADER);
    }
    
    bool
    has_headers()
    { return _header != nullptr; }

    void
    SET_fields(const string& fields)
    {
        if (is_closed())
            throw CurlException("connection/handle has been closed");

        if( !fields.empty() )
            set_option(CURLOPT_COPYPOSTFIELDS, fields.c_str());
    }


    void
    SET_fields(const vector<pair<string, string>>& fields)
    { SET_fields( pairs_to_fields_str(fields) ); }

    
    void
    RESET_options()
    {
        RESET_headers();
        if (_handle) {
            curl_easy_reset(_handle);
        }
        _options.clear();
    }

    ostream&
    to_out(ostream& out);
};


CurlConnection::CurlConnectionImpl_::Init CurlConnection::CurlConnectionImpl_::_init;

template<typename T, typename Dummy>
struct CurlConnection::CurlConnectionImpl_::to {
    static string str(T t)
    { return std::to_string(t); }
};

template<typename Dummy>
struct CurlConnection::CurlConnectionImpl_::to<const char*, Dummy> {
    static string str(const char* s)
    { return string(s); }
};

template<typename T, typename Dummy>
struct CurlConnection::CurlConnectionImpl_::to<T*, Dummy> {
    static string str(T* p)
    { return std::to_string(reinterpret_cast<unsigned long long>(p)); }
};


CurlConnection::CurlConnection()
    : _pimpl( new CurlConnectionImpl_() )
    {
    }

CurlConnection::CurlConnection(string url)
    : _pimpl( new CurlConnectionImpl_(url) )
    {
    }


CurlConnection::CurlConnection( CurlConnection&& connection )
    : _pimpl( connection._pimpl )
    { connection._pimpl = nullptr; }

CurlConnection&
CurlConnection::operator=( CurlConnection&& connection )
{
    if(*this != connection){
        _pimpl = connection._pimpl;
        connection._pimpl = nullptr;
    }
    return *this;
}

CurlConnection::~CurlConnection()
{ if(_pimpl) delete _pimpl; }

bool
CurlConnection::operator==( const CurlConnection& connection )
{
    if( _pimpl != connection._pimpl ) // different objs
        return false;

    if( !_pimpl ) // both null
        return true;

    return _pimpl->operator==(*connection._pimpl); // logically ==
}

bool
CurlConnection::operator!=( const CurlConnection& connection )
{ return !operator==(connection); }

const map<CURLoption, string>&
CurlConnection::get_option_strings() const
{ return _pimpl->get_option_strings(); }

template<typename T>
void
CurlConnection::set_option(CURLoption option, T param)
{ _pimpl->set_option(option, param); }

void
CurlConnection::reset_options()
{ _pimpl->RESET_options(); }

// <status code, data, time>
tuple<long, string, string, clock_ty::time_point>
CurlConnection::execute( bool return_header_data )
{ return _pimpl->execute(return_header_data); }

void
CurlConnection::close()
{ _pimpl->close(); }

bool
CurlConnection::is_closed() const
{ return _pimpl->is_closed(); }


CurlConnection::operator bool() const
{ return _pimpl->operator bool(); }

void
CurlConnection::set_url(string url)
{ _pimpl->SET_url(url); }

string
CurlConnection::get_url() const
{ return _pimpl->GET_url(); }

void
CurlConnection::CurlConnection::set_ssl_verify(bool on)
{ _pimpl->SET_ssl_verify(on); }

void
CurlConnection::set_ssl_verify_using_ca_bundle(string path)
{ _pimpl->SET_ssl_verify_using_ca_bundle(path); }

void
CurlConnection::set_ssl_verify_using_ca_certs(string dir)
{ _pimpl->SET_ssl_verify_using_ca_certs(dir); }

void
CurlConnection::set_encoding(string enc)
{ _pimpl->SET_encoding(enc); }

void
CurlConnection::set_keepalive(bool on)
{ _pimpl->SET_keepalive(on); }

void
CurlConnection::set_timeout(long timeout)
{ _pimpl->SET_timeout(timeout); }

long
CurlConnection::get_timeout() const
{ return _pimpl->GET_timeout(); }

void
CurlConnection::add_headers(const vector<pair<string,string>>& headers)
{ _pimpl->ADD_headers(headers); }

vector<pair<string,string>>
CurlConnection::get_headers() const
{ return _pimpl->GET_headers(); }

void
CurlConnection::reset_headers()
{ _pimpl->RESET_headers(); }

bool
CurlConnection::has_headers()
{ return _pimpl->has_headers(); }

void
CurlConnection::set_fields(const vector<pair<string, string>>& fields)
{ _pimpl->SET_fields(fields); }

void
CurlConnection::set_fields(const string& fields)
{ _pimpl->SET_fields(fields); }

/*
 * If empty certificate_bundle_path(default) curl uses the default store.
 * If that works w/ openssl great, otherwise we'll fail w/ CURLE_SSL_CACERT
 */
std::string certificate_bundle_path;


const string HTTPConnection::DEFAULT_ENCODING("gzip");

HttpMethod
HTTPConnection::_set_method(HttpMethod meth)
{
    switch( meth ){
    case HttpMethod::http_get:
        set_option(CURLOPT_HTTPGET, 1L);
        break;
    case HttpMethod::http_post:
        set_option(CURLOPT_POST, 1L);
        break;
    case HttpMethod::http_delete:
        set_option(CURLOPT_CUSTOMREQUEST, "DELETE");
        break;
    case HttpMethod::http_put:
        set_option(CURLOPT_CUSTOMREQUEST, "PUT");
        break;
    default:
        throw std::runtime_error("invalid HttpMethod");
    }
    return meth;
}


HTTPConnection::HTTPConnection( const std::string& url,
                                HttpMethod meth )
    :
        CurlConnection(), // do manual url check in set_url
        _proto(Protocol::none),
        _meth( _set_method(meth) )
    {
        set_encoding(DEFAULT_ENCODING);
        set_keepalive(true);
        set_url(url);
    }

HTTPConnection::HTTPConnection( HttpMethod meth )
    :
        CurlConnection(),
        _proto(Protocol::none),
        _meth( _set_method(meth) )
    {
        set_encoding(DEFAULT_ENCODING);
        set_keepalive(true);
    }


void
HTTPConnection::set_url(const std::string& url)
{
    if( url.rfind("https://",0) == 0 ){
        if(_proto != Protocol::https ){
            if( certificate_bundle_path.empty() )
                _pimpl->SET_ssl_verify(true);
            else
                _pimpl->SET_ssl_verify_using_ca_bundle(certificate_bundle_path);
            _proto = Protocol::https;
        }
    }else if( url.rfind("http://", 0) == 0 )
        _proto = Protocol::http;
    else
        throw CurlException("invalid protocol in url: " + url);

    _pimpl->SET_url(url);
}


std::unordered_map<int, SharedHTTPConnection::Context> SharedHTTPConnection::contexts;
std::mutex SharedHTTPConnection::contexts_mtx;

SharedHTTPConnection::SharedHTTPConnection( const std::string& url,
                                            HttpMethod meth,
                                            int context_id )
    :
        _is_open(false),
        _url(url),
        _meth(meth),
        _headers(),
        _fields(),
        _timeout(0),
        _id(context_id)
    {
        { // all 'opening' context ops should hold static mutex
            std::lock_guard<std::mutex> lock(contexts_mtx);

            auto citer = contexts.find(context_id);
            if( citer == contexts.cend() ){
                // if new id add a context with new connection
                contexts[context_id] = Context(
                        url.empty() ? new HTTPConnection(meth)
                                    : new HTTPConnection(url,meth)
                );
            }else{
                assert( citer->second.nref > 0 );
                {
                    // if old but not all have been closed
                    std::lock_guard<std::mutex> lock( *(citer->second.mtx) );
                    citer->second.conn->set_method(meth);
                    // set url (redundant) to force ssl verify check
                    if( !url.empty() )
                        citer->second.conn->set_url(url);
                }
            }
            incr_ref(context_id);
        }
        _is_open = true;
    }


SharedHTTPConnection::SharedHTTPConnection( const SharedHTTPConnection& connection )
    :
        _is_open( connection._is_open ),
        _url( connection._url ),
        _meth( connection._meth),
        _headers( connection._headers ),
        _fields( connection._fields ),
        _timeout( connection._timeout ),
        _id( connection._id )
    {
        if( _is_open ){
            std::lock_guard<std::mutex> lock(contexts_mtx);
            incr_ref(connection._id);
        }
    }


SharedHTTPConnection&
SharedHTTPConnection::operator=( const SharedHTTPConnection& connection )
{
    if( *this != connection ){
        {
            std::lock_guard<std::mutex> lock(contexts_mtx);
            if( _is_open != connection._is_open ){
                if( connection._is_open )
                    incr_ref(connection._id);
                else
                    decr_ref(_id);
            }else if( _id != connection._id && _is_open ){
                decr_ref(_id);
                incr_ref(connection._id);
            }
        }
        _is_open = connection._is_open;
        _url = connection._url;
        _meth = connection._meth;
        _headers = connection._headers;
        _fields = connection._fields;
        _timeout = connection._timeout;
        _id = connection._id;
    }
    return *this;
}

bool
SharedHTTPConnection::operator==( const SharedHTTPConnection& connection )
{
    return _is_open == connection._is_open
            && _url == connection._url
            && _meth == connection._meth
            && _headers == connection._headers
            && _fields == connection._fields
            && _timeout == connection._timeout
            && _id == connection._id;

}

std::tuple<long, std::string, std::string, clock_ty::time_point>
SharedHTTPConnection::execute(bool return_header_data)
{
    if( is_closed() )
        throw CurlException("connection has been closed");

    Context& ctx = _get_context();
    assert( ctx.conn );
    assert( ctx.mtx );

    // protect from concurrent access by other shared connections
    std::lock_guard<std::mutex> lock( *ctx.mtx );

    ctx.conn->set_url(_url);

    ctx.conn->reset_headers();
    if( !_headers.empty() )
        ctx.conn->add_headers(_headers);

    ctx.conn->set_method(_meth);
    if( _meth != HttpMethod::http_get && !_fields.empty() )
        ctx.conn->set_fields(_fields);
    _fields.clear();

    ctx.conn->set_timeout(_timeout);

    return ctx.conn->execute(return_header_data);
}

SharedHTTPConnection::Context&
SharedHTTPConnection::_get_context() const
{
    std::lock_guard<std::mutex> lock(contexts_mtx);
    /*
     * only hold the static mutex for the find - if open the only
     *   shared-state at risk of data race is nref which we don't need
     */
    auto citer = contexts.find(_id);
    assert( citer != contexts.cend() );
    return citer->second;
}

void
SharedHTTPConnection::close()
{
    if( is_closed() )
        return;
    {
        std::lock_guard<std::mutex> lock(contexts_mtx);
        decr_ref(_id);
    }
    _is_open = false;
}

void
SharedHTTPConnection::set_url(const std::string& url)
{
    using namespace std::regex_constants;

    static const std::regex PROTO_RX("http[s]?\\://");

    if( !std::regex_search(url, PROTO_RX, match_continuous) )
        throw CurlException("invalid protocol in url: " + url);

    _url = url;
}


// caller needs to hold static mutex
void
SharedHTTPConnection::decr_ref(int id)
{
    auto citer = contexts.find(id);
    assert( citer != contexts.cend() );
    assert( citer->second.conn );
    --(citer->second.nref);
    assert(citer->second.nref >= 0);
    if( citer->second.nref == 0 )
        contexts.erase(citer);
}

// caller needs to hold static mutex
void
SharedHTTPConnection::incr_ref(int id)
{
    auto citer = contexts.find(id);
    assert( citer != contexts.cend() );
    assert( citer->second.conn );
    ++(citer->second.nref);
}

int
SharedHTTPConnection::nconnections(int context_id)
{
    std::lock_guard<std::mutex> lock(contexts_mtx);
    auto citer = contexts.find(context_id);
    return (citer == contexts.end()) ? 0 : citer->second.nref;
}

CurlException::CurlException(string what)
    :
        _what(what)
    {}

const char*
CurlException::what() const noexcept
{ return _what.c_str(); }


CurlOptionException::CurlOptionException(CURLoption opt, string val)
    :
        CurlException( "error setting easy curl option(" + std::to_string(opt)
                       + ") with value(" + val + ")"),
        option(opt),
        value(val)
    {}


CurlOptionException::CurlOptionException(string what, CURLoption opt, string val)
    :
        CurlException(what),
        option(opt),
        value(val)
    {}


CurlConnectionError::CurlConnectionError(CURLcode code, const std::string& msg)
    :
        CurlException("curl connection error (" + std::to_string(code) + "): "
                + msg),
        code(code)
    {}


std::string
pairs_to_fields_str(const std::vector<std::pair<std::string, std::string>>& fields)
{
    std::stringstream ss;
    for (auto& f : fields)
        ss << f.first << "=" << f.second << "&";

    std::string s = ss.str();
    if ( !s.empty() )
        s.erase(s.end() - 1, s.end());

    return s.c_str();
}


vector<pair<string, string>>
fields_str_to_map(const string& fstr)
{
    static const string C{'&'};

    vector<pair<string, string>> res;
    auto b = fstr.cbegin();
    do{
        auto e = find_first_of(b, fstr.cend(), C.cbegin(), C.cend());
        string s(b,e);
        if( !s.empty() ){
            auto s_b = s.cbegin();
            auto s_e = s.cend();
            auto sep = find(s_b, s_e,'=');
            if( sep != s_e ){
                string k(s_b,sep);
                string v(sep+1, s_e);
                res.emplace_back(k,v);
            }
        }
        if( e == fstr.cend() )
            break; // avoid b going past end
        b = e + 1;
    }while(true);

    return res;
}


vector<pair<string, string>>
header_list_to_map(struct curl_slist *hlist)
{
    vector<pair<string, string>> res;
    while( hlist ){
        string s(hlist->data);
        auto i = find(s.cbegin(), s.cend(),':');
        string k(s.cbegin(),i);
        string v(i+1, s.cend());
        res.emplace_back(k, v);
        hlist = hlist->next;
    }
    return res;
}


ostream&
operator<<(ostream& out, const CurlConnection& session)
{ return session._pimpl->to_out(out); }

ostream&
CurlConnection::CurlConnectionImpl_::to_out(ostream& out)
{
    using std::endl;

    for (auto& opt : get_option_strings()) {

        auto oiter = option_strings.find(opt.first);
        if (oiter == option_strings.end()) {
            out << "\tUNKNOWN" << endl;
            continue;
        }

        switch (opt.first) {
        case CURLOPT_COPYPOSTFIELDS:
            out << "\t" << oiter->second << ":" << endl;
            for (auto p : fields_str_to_map(opt.second)) {
                out << "\t\t" << p.first << "\t" << p.second << endl;
            }
            continue;
        case CURLOPT_HTTPHEADER:
            out << "\t" << oiter->second << ":" << endl;
            for (auto p : header_list_to_map(_header)) {
                out << "\t\t" << p.first << "\t" << p.second << endl;
            }
            continue;
        case CURLOPT_WRITEDATA:
        case CURLOPT_WRITEFUNCTION:
            out << "\t" << oiter->second << "\t" << std::hex
                << stoull(opt.second) << std::dec << endl;
            continue;
        default:
            out << "\t" << oiter->second << "\t" << opt.second << endl;
        }
    }

    return out;
}

const map<CURLoption, string> CurlConnection::option_strings = {
    { CURLOPT_SSL_VERIFYPEER, "CURLOPT_SSL_VERIFYPEER"},
    { CURLOPT_SSL_VERIFYHOST, "CURLOPT_SSL_VERIFYHOST"},
    { CURLOPT_CAINFO, "CURLOPT_CAINFO"},
    { CURLOPT_CAPATH, "CURLOPT_CAPATH"},
    { CURLOPT_URL, "CURLOPT_URL"},
    { CURLOPT_ACCEPT_ENCODING, "CURLOPT_ACCEPT_ENCODING"},
    { CURLOPT_TCP_KEEPALIVE, "CURLOPT_TCP_KEEPALIVE"},
    { CURLOPT_HTTPGET, "CURLOPT_HTTPGET"},
    { CURLOPT_POST, "CURLOPT_POST"},
    { CURLOPT_COPYPOSTFIELDS, "CURLOPT_COPYPOSTFIELDS"},
    { CURLOPT_WRITEFUNCTION, "CURLOPT_WRITEFUNCTION"},
    { CURLOPT_WRITEDATA, "CURLOPT_WRITEDATA"},
    { CURLOPT_HTTPHEADER, "CURLOPT_HTTPHEADER"},
    { CURLOPT_NOSIGNAL, "CURLOPT_NOSIGNAL"},
    { CURLOPT_CUSTOMREQUEST, "CURLOPT_CUSTOMREQUEST" },
    { CURLOPT_TIMEOUT_MS, "CURLOPT_TIMEOUT_MS" }
		//{ CURLSSLOPT_NO_REVOKE , "CURLSSLOPT_NO_REVOKE" }
};


void
set_certificate_bundle_path( const std::string& path )
{ certificate_bundle_path = path; }

std::string
get_certificate_bundle_path()
{ return certificate_bundle_path; }

} /* conn */
