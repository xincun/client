/****************************************
 * class	: CClient
 * author	: LittleKu (L.K)
 * email	: kklvzl@gmail.com
 * date		: 09-02-2014
 ****************************************/

#ifndef __CLIENT_H__
#define __CLIENT_H__

#pragma once

#include <boost/bind.hpp>
#include <boost/asio.hpp>
#include <boost/thread.hpp>
#include <boost/function.hpp>
#include <boost/shared_ptr.hpp>

#include <string>

#include "ClientImpl.h"

namespace client
{
	class CClient
		:private boost::noncopyable
	{
	public:
		CClient();
		~CClient();

		void Init(const std::string &host, const std::string &port, cb_InitConnection cb, int thread_count = 1, int connection_count = 1, int connection_limit = 5);
		void Stop();
		void PostSend(CMsgBuffer::Ptr msg, cb_Request cb);
		void Reset();
		bool IsConnected();
	private:
		boost::shared_ptr<CClientImpl> m_pClientImpl;
		cb_InitConnection m_cbInit;
		std::string m_host;
		std::string m_port;
	};
}

#endif