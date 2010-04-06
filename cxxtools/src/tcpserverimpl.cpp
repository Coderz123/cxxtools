/*
 * Copyright (C) 2003 Tommi Maekitalo
 * 
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * As a special exception, you may use this file as part of a free
 * software library without restriction. Specifically, if other files
 * instantiate templates or use macros or inline functions from this
 * file, or you compile this file and link it with other files to
 * produce an executable, this file does not by itself cause the
 * resulting executable to be covered by the GNU General Public
 * License. This exception does not however invalidate any other
 * reasons why the executable file might be covered by the GNU Library
 * General Public License.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <cxxtools/net/addrinfo.h>
#include "tcpserverimpl.h"
#include "tcpsocketimpl.h"
#include "addrinfoimpl.h"
#include <cxxtools/net/tcpserver.h>
#include <cxxtools/net/tcpsocket.h>
#include <cxxtools/net/net.h>
#include <cxxtools/systemerror.h>
#include <cxxtools/log.h>
#include <cxxtools/ioerror.h>
#include <cerrno>
#include <cassert>
#include <cstring>
#include <sys/poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits>
#include "error.h"

#ifdef HAVE_TCP_DEFER_ACCEPT
#  include <netinet/tcp.h>
#  include <sys/types.h>
#  include <sys/socket.h>
#endif

log_define("cxxtools.net.tcpserverimpl")

namespace cxxtools
{

namespace net
{

static const int noPendingAccept = std::numeric_limits<int>::max();

TcpServerImpl::TcpServerImpl(TcpServer& server)
: _server(server),
  _pendingAccept(noPendingAccept),
  _pfd(0),
  _deferAccept(false)
{

}


int TcpServerImpl::create(int domain, int type, int protocol)
{
    log_debug("create socket");
    int fd = ::socket(domain, type, protocol);
    if (fd < 0)
        throw SystemError("socket");
    return fd;
}


void TcpServerImpl::close()
{
    for (Listeners::const_iterator it = _listeners.begin();
        it != _listeners.end(); ++it)
    {
        if (it->_fd >= 0)
        {
            log_debug("close socket " << it->_fd);
            ::close(it->_fd);
        }
    }

    _listeners.clear();

    _pfd = 0;
#ifdef HAVE_TCP_DEFER_ACCEPT
    _deferAccept = false;
#endif
}


void TcpServerImpl::listen(const std::string& ipaddr, unsigned short int port, int backlog, unsigned flags)
{
    log_debug("listen on " << ipaddr << " port " << port << " backlog " << backlog << " flags " << flags);

    bool inherit = (flags & TcpServer::INHERIT) != 0;

    AddrInfo ai(ipaddr, port, true);

    static const int reuseAddr = 1;

    const char* fn = "";

    // getaddrinfo() may return more than one addrinfo structure, so work
    // them all out
    try
    {
        for (AddrInfoImpl::const_iterator it = ai.impl()->begin(); it != ai.impl()->end(); ++it)
        {
            int fd;
            try
            {
                fn = "create";
                fd = create(it->ai_family, SOCK_STREAM, 0);
            }
            catch (const SystemError&)
            {
                log_debug("could not create socket; errno=" << errno << ": " << strerror(errno));
                continue;
            }

            log_debug("setsockopt SO_REUSEADDR");
            fn = "setsockopt";
            if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuseAddr, sizeof(reuseAddr)) < 0)
            {
                log_debug("could not set SO_REUSEADDR " << fd << "; errno=" << errno << ": " << strerror(errno));
                ::close(fd);
                continue;
            }

            log_debug("bind " << formatIp(*reinterpret_cast<const sockaddr_storage*>(it->ai_addr)));
            fn = "bind";
            if (::bind(fd, it->ai_addr, it->ai_addrlen) != 0)
            {
                log_debug("could not bind " << fd << "; errno=" << errno << ": " << strerror(errno));
                ::close(fd);
                continue;
            }

            log_debug("listen");
            fn = "listen";
            if ( ::listen(fd, backlog) < 0 )
            {
                close();
                continue;
            }

            // save our information
            _listeners.push_back(Listener());
            _listeners.back()._fd = fd;
            std::memmove(&_listeners.back()._servaddr, it->ai_addr, it->ai_addrlen);

            if (!inherit)
            {
                int flags = ::fcntl(fd, F_GETFD);
                flags |= FD_CLOEXEC ;
                fn = "fcntl";
                int ret = ::fcntl(fd, F_SETFD, flags);
                if (ret == -1)
                    throw IOError(getErrnoString("Could not set FD_CLOEXEC"), CXXTOOLS_SOURCEINFO);
            }
        }
    }
    catch (const std::exception& e)
    {
        close();
        throw;
    }

#ifdef HAVE_TCP_DEFER_ACCEPT
    deferAccept(flags & TcpServer::DEFER_ACCEPT);
#endif

    if (_listeners.empty())
    {
        if (errno == EADDRINUSE)
            throw AddressInUse();
        else
            throw SystemError(fn);
    }
}


#ifdef HAVE_TCP_DEFER_ACCEPT
void TcpServerImpl::deferAccept(bool sw)
{
    if (sw == _deferAccept)
        return;

    int deferSecs = sw ? 30 : 0;

    log_debug("set TCP_DEFER_ACCEPT to " << deferSecs);

    for (Listeners::const_iterator it = _listeners.begin();
        it != _listeners.end(); ++it)
    {
        if (::setsockopt(it->_fd, SOL_TCP, TCP_DEFER_ACCEPT,
            &deferSecs, sizeof(deferSecs)) < 0)
            throw cxxtools::SystemError("setsockopt(TCP_DEFER_ACCEPT)");
    }
}
#endif

template <typename T>
class Resetter
{
        T& _t;
        T _sav;

    public:
        explicit Resetter(T& t)
            : _t(t),
              _sav(t)
        { }
        Resetter(T& t, T value)
            : _t(t),
              _sav(value)
        { }
        ~Resetter()
        {
            _t = _sav;
        }
};

bool TcpServerImpl::wait(std::size_t msecs)
{
    log_debug("wait " << msecs);

    Resetter<pollfd*> resetter(_pfd);
    std::vector<pollfd> fds(_listeners.size());
    initializePoll(&fds[0], fds.size());

    log_debug("poll timeout " << msecs);

    while (true)
    {
        int p = ::poll(&fds[0], fds.size(), msecs);
        if (p > 0)
        {
            break;
        }
        else if (p < 0)
        {
            if (errno == EINTR)
                continue;
            log_error("error in poll; errno=" << errno);
            throw SystemError("poll");
        }
        else if (p == 0)
        {
            log_debug("poll timeout (" << msecs << ')');
            throw Timeout();
        }
    }

    return checkPollEvent();
}


void TcpServerImpl::attach(SelectorBase& s)
{
    log_debug("attach to selector");
}


void TcpServerImpl::detach(SelectorBase& s)
{
    log_debug("detach from selector");
    _pfd = 0;
}


std::size_t TcpServerImpl::pollSize() const
{
    return _listeners.size();
}


std::size_t TcpServerImpl::initializePoll(pollfd* pfd, std::size_t pollSize)
{
    assert(pfd != 0);
    assert(pollSize == _listeners.size());

    log_debug("initializePoll " << pollSize);

    for (std::size_t n = 0; n < pollSize; ++n)
    {
        pfd[n].fd = _listeners[n]._fd;
        pfd[n].revents = 0;
        pfd[n].events = POLLIN;
    }

    _pfd = pfd;

    return pollSize;
}


bool TcpServerImpl::checkPollEvent()
{
    assert(_pfd != 0);

    bool ret = false;
    Resetter<int> resetter(_pendingAccept, noPendingAccept);
    for (int n = 0; n < _listeners.size(); ++n)
    {
        if (_pfd[n].revents | POLLIN)
        {
            _pendingAccept = n;
            _server.connectionPending.send(_server);
            ret = true;
        }
    }

    return ret;
}


int TcpServerImpl::accept(int flags, struct sockaddr* sa, socklen_t& sa_len)
{
    Resetter<int> resetter(_pendingAccept);
    if (_pendingAccept == noPendingAccept)
    {
        Resetter<pollfd*> resetter(_pfd);
        std::vector<pollfd> fds(_listeners.size());
        initializePoll(&fds[0], fds.size());

        while (true)
        {
            int p = ::poll(&fds[0], fds.size(), -1);
            if (p > 0)
            {
                break;
            }
            else if (p < 0)
            {
                if (errno == EINTR)
                    continue;
                log_error("error in poll; errno=" << errno);
                throw SystemError("poll");
            }
        }

        for (int n = 0; n < fds.size(); ++n)
        {
            if (fds[n].revents | POLLIN)
            {
                _pendingAccept = n;
                break;
            }
        }

        if (_pendingAccept == noPendingAccept)
        {
            // TODO ???
            // poll reported activity but there is no POLLIN set???
            return -1;
        }
    }
    else if (_pfd != 0)  // should be always true here
    {
        _pfd[_pendingAccept].revents = 0;
    }

    int listenerFd = _listeners[_pendingAccept]._fd;

    log_debug( "accept " << listenerFd << ", " << flags );

    bool inherit = (flags & TcpSocket::INHERIT) != 0;

#ifdef HAVE_TCP_DEFER_ACCEPT
    deferAccept(flags & TcpSocket::DEFER_ACCEPT);
#endif

#ifdef HAVE_ACCEPT4
    int f = SOCK_NONBLOCK;
    if (!inherit)
        f |= SOCK_CLOEXEC;
    int clientFd = ::accept4(listenerFd, sa, &sa_len, f);
    if( clientFd < 0 )
        throw SystemError("accept4");
#else
    int clientFd = ::accept(listenerFd, sa, &sa_len);
    if( clientFd < 0 )
        throw SystemError("accept");
#endif

    log_debug( "accepted on " << listenerFd << " => " << clientFd);

    return clientFd;
}


} // namespace net

} // namespace cxxtools
