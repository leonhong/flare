/**
 *	flared.h
 *
 *	@author	Masaki Fujimoto <fujimoto@php.net>
 *
 *	$Id$
 */
#ifndef	__FLARED_H__
#define	__FLARED_H__

#include "app.h"
#include "handler_request.h"
#include "ini_option.h"
#include "stats_node.h"

namespace gree {
namespace flare {

/**
 *	flared application class
 */
class flared : public app {
public:
	enum					thread_type {
		thread_type_request,
	};

private:
	server*				_server;
	thread_pool*	_thread_pool;
	cluster*			_cluster;

public:
	flared();
	~flared();

	int startup(int argc, char** argv);
	int run();
	int reload();
	int shutdown();

	thread_pool* get_thread_pool() { return this->_thread_pool; };

protected:
	string _get_pid_path();

private:
	int _set_resource_limit();
	int _set_signal_handler();
};

}	// namespace flare
}	// namespace gree

#endif	// __FLARED_H__
// vim: foldmethod=marker tabstop=2 shiftwidth=2 autoindent
