/****************************************
 * author	: LittleKu (L.K)
 * email	: kklvzl@gmail.com
 * date		: 09-02-2014
 ****************************************/

#include "ConnectionPool.h"

namespace client
{
	CConnectionPool::CConnectionPool(boost::asio::io_service &io_service)
		:m_Host(""),
		m_Port(""),
		m_Io_Service(io_service),
		m_TryTimer(io_service),
		m_TryTimerConnect(io_service)
	{
		m_nConnectionCount = 0;
		m_bIsStop = false;
		m_nConnectionLimit = 10;
		m_TryConnect = 0;
		m_TryCount = 0;
		m_TimeoutRequest = 5;
		m_TimeoutConnect = 2;
	}

	CConnectionPool::~CConnectionPool()
	{
	}

	void CConnectionPool::Init(const std::string &host, const std::string &port, cb_InitConnection cb, int connection_count /*= 5*/, int try_count /*= 1*/, int connection_limit /*= 1*/)
	{
		m_Host = host;
		m_Port = port;
		m_TryCount = try_count;
		m_nConnectionLimit = connection_limit;
		
		for (int i = 0; i < connection_count; i++)
		{
			NewConnection(cb);
		}
	}

	void CConnectionPool::Stop()
	{
		boost::mutex::scoped_lock lock(m_Mutex);
		
		m_bIsStop = true;

		while (!m_DequeValid.empty())
		{
			CConnection::Ptr connection = m_DequeValid.front();
			connection->Close();
			m_DequeValid.pop_front();
		}

		while (!m_ListRun.empty())
		{
			CConnection::Ptr connection = m_ListRun.front();
			connection->Close();
			m_ListRun.pop_front();
		}

		while (!m_ListNew.empty())
		{
			CConnection::Ptr connection = m_ListNew.front();
			connection->Close();
			m_ListNew.pop_front();
		}

		m_TryTimer.cancel();
		m_TryTimerConnect.cancel();

		while (!m_DequeRequest.empty())
		{
			CConnection::Ptr connection;
			cb_addConnection cb = m_DequeRequest.front();
			m_DequeRequest.pop_front();
			boost::system::error_code error(boost::asio::error::not_connected);
			cb(error, connection);
		}
	}

	void CConnectionPool::NewConnection(cb_InitConnection cb)
	{
		//系统还不准备与服务器连接,或者已经与服务器断开了连接
		if (m_bIsStop)
			return;

		//申请新的连接对象,并尝试与服务器连接,并当连接成功时等待服务数据包的到来
		CConnection::Ptr connection(new CConnection(m_Io_Service));
		connection->Connect(m_Host, m_Port, boost::bind(&CConnectionPool::QueueConnection, shared_from_this(), _1, _2, _3, cb, connection));
		//新创建的连接对象存入新建连接列表里
		m_ListNew.push_back(connection);

		//创建连接的基数加1
		boost::mutex::scoped_lock lock(m_Mutex);
		m_nConnectionCount++;
	}

	//async_resolve,async_connect,async_read,async_write成功与否,都会调用此函数作为回调函数,并传递错误代码及数据包
	void CConnectionPool::QueueConnection(const boost::system::error_code &err, StatusCode sc, CMessage::Ptr msg, cb_InitConnection cb, CConnection::Ptr connection)
	{
		boost::mutex::scoped_lock lock(m_Mutex);

		//StatusCode可以得知当前触发此回调在哪一个步骤
		//1.CConnection::Handle_Write发送成功或失败时要回收connection
		//2.CConnection::Connect中捕捉到错误,此时的connection由NewConnection传递过来,也在此处理
		//3.CConnection::Handle_Resolver错误处理,此时的connection由NewConnection传递过来,也在此处理
		//4.CConnection::Handle_Connect连接成功或失败时,通知此回调,此时的connection由NewConnection传递过来,也在此处理
		//5.CConnection::Handle_Read_Header错误处理,此时的connection由NewConnection传递过来,也在此处理
		//6.CConnection::Handle_Read_Body成功接收一个完整数据包或接收失败时,通知此回调,此时的connection由NewConnection传递过来,也在此处理

		/**
		 * 除NewConnection时申请内存失败外,此时的connection都不会为空
		 */
		if (connection)
		{
			//除从CConnection::Handle_Write触发时的connection在m_ListRun表里外,其它的都不在m_ListRun,所以无论哪种情况,此时都可以把connection从m_ListRun移除
			m_ListRun.remove(connection);
			//无论连接从哪来传递过来,都不应该再留在新建列表里了,所以如果有在列表里的话,就移除
			m_ListNew.remove(connection);

			//如果用户已经停止与服务器的一切信息交互,那么就直接返回
			if (m_bIsStop)
				return;

			//连接对象的状态是否正常,并且没有任何错误
			if (connection->IsOpen() && !err)
			{
				//将没有问题的连接对象存入等待使用的列表中,以备随时取出使用
				m_DequeValid.push_back(connection);
			}
			//状态有异常或者有错误时
			else
			{
				//查看有效的连接数+正在运行的连接数+新建的连接数是否小于系统设定的最小连接数
				if ( (m_DequeValid.size() + m_ListRun.size() + m_ListNew.size() ) < (size_t)m_nConnectionLimit)
				{
					//尝试继续连接的基数加1
					m_TryCount++;
					//当尝试连接的基数小于3时
					if (m_TryCount < 3)
					{
						//系统继续重新再创建新的连接
						m_Io_Service.post(boost::bind(&CConnectionPool::NewConnection, shared_from_this(), cb));
					}
					//如果尝试连接的基数大于或等于3时
					else
					{
						//此时设置超时等待
						int res = m_TryTimerConnect.expires_from_now(boost::posix_time::seconds(m_TimeoutConnect));
						if (!res)
						{
							m_TryTimerConnect.async_wait(boost::bind(&CConnectionPool::TimeoutNewConnection, shared_from_this(), boost::asio::placeholders::error, cb));
						}
					}
				}
			}
		}

		//有效的连接对象池中不为空,并且请求队列中有等待发送的数据
		if (!m_DequeValid.empty() && !m_DequeRequest.empty())
		{
			//从有效的连接对象池中取出一个可用的连接对象
			CConnection::Ptr pConnection = m_DequeValid.front();
			m_DequeValid.pop_front();

			//把这个有效的连接对象存入正在使用的连接对象池中
			m_ListRun.push_back(pConnection);

			//从发送请求队列中取出一个请求,并发送
			cb_addConnection pCb = m_DequeRequest.front();
			m_DequeRequest.pop_front();

			boost::system::error_code error;
			m_Io_Service.post(boost::bind(pCb, error, pConnection));
		}

		if (cb && msg)
		{
			boost::system::error_code error;
			if (m_DequeValid.empty())
				error = boost::asio::error::not_connected;

			m_Io_Service.post(boost::bind(cb, error, msg));
		}
	}

	void CConnectionPool::GetConnection(cb_addConnection cb)
	{
		boost::mutex::scoped_lock lock(m_Mutex);
		if (!m_DequeValid.empty())
		{
			CConnection::Ptr connection = m_DequeValid.front();
			m_DequeValid.pop_front();
			m_ListRun.push_back(connection);
			boost::system::error_code error;
			m_Io_Service.post(boost::bind(cb, error, connection));
		}
		else
		{
			m_DequeRequest.push_back(cb);

			int res = m_TryTimer.expires_from_now(boost::posix_time::seconds(m_TimeoutRequest));
			if (!res)
			{
				m_TryTimer.async_wait(boost::bind(&CConnectionPool::CheckAvaliableConnection, shared_from_this(), boost::asio::placeholders::error));
			}
		}
	}

	void CConnectionPool::CheckAvaliableConnection(const boost::system::error_code &err)
	{
		bool aborted = (err == boost::asio::error::operation_aborted) ? true : false;
		if (aborted)
			return;

		boost::mutex::scoped_lock lock(m_Mutex);

		CConnection::Ptr connection;
		boost::system::error_code error;

		if (!m_DequeRequest.empty())
		{
			cb_addConnection cb = m_DequeRequest.front();
			m_DequeRequest.pop_front();

			if (!m_DequeValid.empty())
			{
				connection = m_DequeValid.front();
				m_DequeValid.pop_front();
				m_ListRun.push_back(connection);
			}
			else
			{
				error = boost::asio::error::not_connected;
			}
			m_Io_Service.post(boost::bind(cb, error, connection));
		}
	}

	void CConnectionPool::TimeoutNewConnection(const boost::system::error_code &err, cb_InitConnection cb)
	{
		//连接超时,如果是因为用户终止连接,那么就直接返回
		bool aborted = (err == boost::asio::error::operation_aborted) ? true : false;
		if (aborted)
			return;

		//否则就清除尝试连接基数,重新再尝试连接
		boost::mutex::scoped_lock lock(m_Mutex);

		m_TryConnect = 0;
		m_Io_Service.post(boost::bind(&CConnectionPool::NewConnection, shared_from_this(), cb));
	}
}