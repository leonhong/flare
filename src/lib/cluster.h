/**
 *	cluster.h
 *
 *	@author	Masaki Fujimoto <fujimoto@php.net>
 *
 *	$Id$
 */
#ifndef	__CLUSTER_H__
#define	__CLUSTER_H__

#include <fstream>
#include <map>
#include <stack>
#include <vector>

#include <boost/archive/xml_oarchive.hpp>
#include <boost/archive/xml_iarchive.hpp>
#include <boost/serialization/serialization.hpp>
#include <boost/serialization/nvp.hpp>
#include <boost/serialization/map.hpp>
#include <boost/serialization/string.hpp>
#include <boost/lexical_cast.hpp>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "connection.h"
#include "storage.h"
#include "thread_pool.h"
#include "key_resolver.h"

using namespace std;
using namespace boost;

namespace gree {
namespace flare {

typedef class op_proxy_write op_proxy_write;
typedef class op_proxy_read op_proxy_read;

typedef class queue_proxy_read queue_proxy_read;
typedef shared_ptr<queue_proxy_read> shared_queue_proxy_read;

typedef class queue_proxy_write queue_proxy_write;
typedef shared_ptr<queue_proxy_write> shared_queue_proxy_write;

/**
 *	cluster class
 */
class cluster {
public:
	enum							type {
		type_index,
		type_node,
	};

	enum							replication {
		replication_async,
		replication_sync,
	};

	enum							role {
		role_master,
		role_slave,
		role_proxy,
	};

	enum							state {
		state_active,
		state_prepare,
		state_down,
	};

	enum							proxy_request {
		proxy_request_error_partition = -2,
		proxy_request_error_enqueue = -1,
		proxy_request_continue = 1,
		proxy_request_complete,
	};


	typedef struct		_node {
		string					node_server_name;
		int							node_server_port;
		role						node_role;
		state						node_state;
		int							node_partition;
		int							node_balance;
		int							node_thread_type;

		int parse(const char* p);

	private:
		friend class serialization::access;
		template<class T> void serialize(T& ar, const unsigned int version) {
			ar & BOOST_SERIALIZATION_NVP(node_server_name);
			ar & BOOST_SERIALIZATION_NVP(node_server_port);
			ar & BOOST_SERIALIZATION_NVP(node_role);
			ar & BOOST_SERIALIZATION_NVP(node_state);
			ar & BOOST_SERIALIZATION_NVP(node_partition);
			ar & BOOST_SERIALIZATION_NVP(node_balance);
			ar & BOOST_SERIALIZATION_NVP(node_thread_type);
		};
	} node;

	typedef struct		_partition_node {
		string					node_key;
		int							node_balance;
	} partition_node;

	typedef struct		_partition {
		partition_node					master;
		vector<partition_node>	slave;
		vector<string>					balance;
		map<string, bool>				index;
	} partition;

	typedef struct _node_shift_state {
		string node_key;
		state old_state;
		state new_state;
	} node_shift_state;

	typedef struct _node_shift_role {
		string node_key;
		role old_role;
		int old_partition;
		role new_role;
		int new_partition;
	} node_shift_role;
	
	typedef map<string, node>		node_map;
	typedef map<int, partition>	node_partition_map;

	static const int	default_thread_type = 16;
	static const int	max_partition_size = 1024;

protected:
	thread_pool*					_thread_pool;
	key_resolver*					_key_resolver;
	storage*							_storage;
	type									_type;
	string								_data_dir;
	pthread_mutex_t				_mutex_serialization;

	node_map							_node_map;
	node_partition_map		_node_partition_map;
	node_partition_map		_node_partition_prepare_map;
	pthread_rwlock_t			_mutex_node_map;
	pthread_rwlock_t			_mutex_node_partition_map;

	string								_node_key;
	string								_server_name;
	int										_server_port;

	// [index]
	int										_monitor_threshold;
	int										_monitor_interval;
	int										_monitor_read_timeout;
	int										_thread_type;
#ifdef ENABLE_MYSQL_REPLICATION
	bool									_mysql_replication;
#endif

	// [node]
	string								_index_server_name;
	int										_index_server_port;
	int										_proxy_concurrency;
	int										_reconstruction_interval;
	replication						_replication_type;

public:
	cluster(thread_pool* tp, string data_dir, string server_name, int server_port);
	virtual ~cluster();

	int startup_index(key_resolver::type key_resolver_type, int key_resolver_modular_hint);
	int startup_node(string index_server_name, int index_server_port);

	int add_node(string node_server_name, int node_server_port);
	int down_node(string node_server_name, int node_server_port);
	int up_node(string node_server_name, int node_server_port, bool force = true);
	int remove_node(string node_server_name, int node_server_port);
	int set_node_role(string node_server_name, int node_server_port, role node_role, int node_balance, int node_partition);
	int set_node_state(string node_server_name, int node_server_port, state node_state);
	int reconstruct_node(vector<node> v);

	int set_storage(storage* st) { this->_storage = st; return 0; };

	int activate_node();
	int deactivate_node();
	proxy_request pre_proxy_read(op_proxy_read* op, storage::entry& e, void* parameter, shared_queue_proxy_read& q);
	proxy_request pre_proxy_write(op_proxy_write* op, shared_queue_proxy_write& q, uint64_t generic_value = 0);
	proxy_request post_proxy_write(op_proxy_write* op, bool sync = false);

	inline node get_node(string node_key) {
		node n;
		pthread_rwlock_rdlock(&this->_mutex_node_map);
		if (this->_node_map.count(node_key) > 0) {
			n = this->_node_map[node_key];
		} else {
			n.node_server_name = "";
			n.node_server_port = 0;
		}
		pthread_rwlock_unlock(&this->_mutex_node_map);

		return n;
	};
	inline node get_node(string node_server_name, int node_server_port) {
		return this->get_node(this->to_node_key(node_server_name, node_server_port));
	}
	vector<node> get_node();
	vector<node> get_slave_node();

	key_resolver* get_key_resolver() { return this->_key_resolver; };
	int set_monitor_threshold(int monitor_threshold);
	int set_monitor_interval(int monitor_interval);
	int set_monitor_read_timeout(int monitor_read_timeout);
	int set_proxy_concurrency(int proxy_concurrency) { this->_proxy_concurrency = proxy_concurrency; return 0; };
	int get_reconstruction_interval() { return this->_reconstruction_interval; };
	int set_reconstruction_interval(int reconstruction_interval) { this->_reconstruction_interval = reconstruction_interval; return 0; };
	replication get_replication_type() { return this->_replication_type; };
	int set_replication_type(string replication_type) { cluster::replication_cast(replication_type, this->_replication_type); return 0; };
	string get_server_name() { return this->_server_name; };
	int get_server_port() { return this->_server_port; };
	string get_index_server_name() { return this->_index_server_name; };
	int get_index_server_port() { return this->_index_server_port; };

#ifdef ENABLE_MYSQL_REPLICATION
	int set_mysql_replication(bool mysql_replication) { this->_mysql_replication = mysql_replication; return 0; };
	bool is_mysql_replication() { return this->_mysql_replication; };
#endif

	inline string to_node_key(string server_name, int server_port) {
		string node_key = server_name + ":" + lexical_cast<string>(server_port);
		return node_key;
	};

	inline int from_node_key(string node_key, string& server_name, int& server_port) {
		string::size_type n = node_key.find(":", 0);
		if (n == string::npos) {
			return -1;
		}
		server_name = node_key.substr(0, n);
		try {
			server_port = lexical_cast<int>(node_key.substr(n+1));
		} catch (bad_lexical_cast e) {
			return -1;
		}
		return 0;
	}

	static inline int replication_cast(string s, replication& t) {
		if (s == "async") {
			t = replication_async;
		} else if (s == "sync") {
			t = replication_sync;
		} else {
			return -1;
		}
		return 0;
	}

	static inline string replication_cast(replication t) {
		switch (t) {
		case replication_async:
			return "async";
		case replication_sync:
			return "sync";
		}
		return "";
	}

	static inline int role_cast(string s, role& r) {
		if (s == "master") {
			r = role_master;
		} else if (s == "slave") {
			r = role_slave;
		} else if (s == "proxy") {
			r = role_proxy;
		} else {
			return -1;
		}
		return 0;
	}

	static inline string role_cast(role r) {
		switch (r) {
		case role_master:
			return "master";
		case role_slave:
			return "slave";
		case role_proxy:
			return "proxy";
		}
		return "";
	}

	static inline int state_cast(string s, state& t) {
		if (s == "active") {
			t = state_active;
		} else if (s == "prepare") {
			t = state_prepare;
		} else if (s == "down") {
			t = state_down;
		} else {
			return -1;
		}
		return 0;
	}

	static inline string state_cast(state t) {
		switch (t) {
		case state_active:
			return "active";
		case state_prepare:
			return "prepare";
		case state_down:
			return "down";
		}
		return "";
	}

protected:
	int _shift_node_state(string node_key, state old_state, state new_state);
	int _shift_node_role(string node_key, role old_role, int old_partition, role new_role, int new_partition);
	int _enqueue(shared_thread_queue q, string node_key, int key_hash, bool sync = false);
	int _enqueue(shared_thread_queue q, thread_pool::thread_type t, bool sync);
	int _broadcast(shared_thread_queue q, bool sync = false);
	int _save();
	int _load();
	int _reconstruct_node_partition(bool lock = true);
	int _check_node_balance(string node_key, int node_balance);
	int _check_node_partition(int node_partition, bool& preparing);
	int _check_node_partition_for_new(int node_partition, bool& preparing);
	int _determine_partition(storage::entry& e, partition& p, bool include_prepare, bool& is_preprare);
	string _get_partition_key(string key);
	int _get_proxy_thread(string node_key, int key_hash, shared_thread& t);
};

}	// namespace flare
}	// namespace gree

#endif	// __CLUSTER_H__
// vim: foldmethod=marker tabstop=2 shiftwidth=2 autoindent
