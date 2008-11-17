/**
 *	storage_tch.cc
 *
 *	implementation of gree::flare::storage_tch
 *
 *	@author	Masaki Fujimoto <fujimoto@php.net>
 *
 *	$Id$
 */
#include "app.h"
#include "storage_tch.h"

namespace gree {
namespace flare {

// {{{
// }}}

// {{{ ctor/dtor
/**
 *	ctor for storage_tch
 */
storage_tch::storage_tch(string data_dir, int mutex_slot_size):
		storage(data_dir, mutex_slot_size),
		_iter_lock(0) {
	this->_data_path = this->_data_dir + "/flare.hdb";

	this->_db = tchdbnew();
	tchdbsetmutex(this->_db);
}

/**
 *	dtor for storage
 */
storage_tch::~storage_tch() {
	if (this->_open) {
		this->close();
	}
	if (this->_db != NULL) {
		tchdbdel(this->_db);
	}
}
// }}}

// {{{ operator overloads
// }}}

// {{{ public methods
int storage_tch::open() {
	if (this->_open) {
		log_warning("storage has been already opened", 0);
		return -1;
	}

	if (tchdbopen(this->_db, this->_data_path.c_str(), HDBOWRITER | HDBOCREAT) == false) {
		int ecode = tchdbecode(this->_db);
		log_err("tchdbopen() failed: %s (%d)", tchdberrmsg(ecode), ecode);
		return -1;
	}

	log_debug("storage open (path=%s, type=%s)", this->_data_path.c_str(), storage::type_cast(this->_type).c_str());
	this->_open = true;

	return 0;
}

int storage_tch::close() {
	if (this->_open == false) {
		log_warning("storage is not yet opened", 0);
		return -1;
	}

	if (tchdbclose(this->_db) == false) {
		int ecode = tchdbecode(this->_db);
		log_err("tchdbclose() failed: %s (%d)", tchdberrmsg(ecode), ecode);
		return -1;
	}

	log_debug("storage close", 0);
	this->_open = false;

	return 0;
}

int storage_tch::set(entry& e, result& r, int b) {
	log_info("set (key=%s, flag=%d, expire=%ld, size=%llu, version=%llu", e.key.c_str(), e.flag, e.expire, e.size, e.version);
	int mutex_index = 0;

	if ((b & behavior_skip_lock) == 0) {
		mutex_index = e.get_key_hash_value(hash_algorithm_bitshift) % this->_mutex_slot_size;
	}

	uint8_t* p = NULL;
	try {
		if ((b & behavior_skip_lock) == 0) {
			pthread_rwlock_wrlock(&this->_mutex_slot[mutex_index]);
		}

		entry e_current;
		int e_current_exists = this->_get_header(e.key, e_current);
		
		// check for "add"
		if ((b & behavior_add) != 0 && e_current_exists == 0) {
			log_debug("behavior=add and data exists -> skip setting", 0);
			r = result_not_stored;
			throw 0;
		}

		// check for "replace"
		if ((b & behavior_replace) != 0 && e_current_exists != 0) {
			log_debug("behavior=replace and data not found -> skip setting", 0);
			r = result_not_stored;
			throw 0;
		}

		if ((b & behavior_skip_version) == 0 && e.version != 0) {
			if (e.version <= e_current.version) {
				log_info("specified version is older than (or equal to) current version -> skip setting (current=%u, specified=%u)", e_current.version, e.version);
				r = result_not_stored;
				throw 0;
			}
		}

		if (e.version == 0) {
			e.version = e_current.version+1;
			log_debug("updating version (version=%llu)", e.version);
		}
		p = _new_ uint8_t[entry::header_size + e.size];
		this->_serialize_header(e, p);
		memcpy(p+entry::header_size, e.data.get(), e.size);

		if (e.option & option_noreply) {
			if (tchdbputasync(this->_db, e.key.c_str(), e.key.size(), p, entry::header_size + e.size) == true) {
				log_debug("stored data [async] (key=%s, size=%llu)", e.key.c_str(), entry::header_size + e.size);
				r = result_stored;
			} else {
				int ecode = tchdbecode(this->_db);
				log_err("tchdbputasync() failed: %s (%d)", tchdberrmsg(ecode), ecode);
				throw -1;
			}
		} else {
			if (tchdbput(this->_db, e.key.c_str(), e.key.size(), p, entry::header_size + e.size) == true) {
				log_debug("stored data [sync] (key=%s, size=%llu)", e.key.c_str(), entry::header_size + e.size);
				r = result_stored;
			} else {
				int ecode = tchdbecode(this->_db);
				log_err("tchdbput() failed: %s (%d)", tchdberrmsg(ecode), ecode);
				throw -1;
			}
		}
	} catch (int error) {
		if (p) {
			_delete_(p);
		}
		if ((b & behavior_skip_lock) == 0) {
			pthread_rwlock_unlock(&this->_mutex_slot[mutex_index]);
		}
		return error;
	}
	if (p) {
		_delete_(p);
	}
	if ((b & behavior_skip_lock) == 0) {
		pthread_rwlock_unlock(&this->_mutex_slot[mutex_index]);
	}

	return 0;
}

int storage_tch::get(entry& e, result& r, int b) {
	log_info("get (key=%s)", e.key.c_str());
	int mutex_index = 0;
	uint8_t* tmp_data = NULL;
	int tmp_len = 0;
	bool remove_request = false;

	if ((b & behavior_skip_lock) == 0) {
		mutex_index = e.get_key_hash_value(hash_algorithm_bitshift) % this->_mutex_slot_size;
	}

	try {
		if ((b & behavior_skip_lock) == 0) {
			log_debug("getting read lock (index=%d)", mutex_index);
			pthread_rwlock_rdlock(&this->_mutex_slot[mutex_index]);
		}

		tmp_data = (uint8_t*)tchdbget(this->_db, e.key.c_str(), e.key.size(), &tmp_len);
		if (tmp_data == NULL) {
			r = result_not_found;
			throw 0;
		}

		int offset = this->_unserialize_header(tmp_data, tmp_len, e);
		if (offset < 0) {
			log_err("failed to unserialize header (data is corrupted) (tmp_len=%d)", tmp_len);
			throw -1;
		}
		if (static_cast<uint64_t>(tmp_len - offset) != e.size) {
			log_err("actual data size is different from header data size (actual=%d, header=%u)", tmp_len-offset, e.size);
			throw -1;
		}

		if ((b & behavior_skip_timestamp) == 0) {
			if (e.expire > 0 && e.expire < stats_object->get_timestamp()) {
				log_info("data expired [expire=%d] -> remove requesting", e.expire);
				r = result_not_found;
				remove_request = true;
				throw 0;
			}
		}

		shared_byte p(new uint8_t[e.size]);
		memcpy(p.get(), tmp_data + offset, tmp_len - offset);
		free(tmp_data);
		e.data = p;
	} catch (int error) {
		if (tmp_data != NULL) {
			free(tmp_data);
		}
		if ((b & behavior_skip_lock) == 0) {
			pthread_rwlock_unlock(&this->_mutex_slot[mutex_index]);
		}

		if (remove_request) {
			result r_remove;
			this->remove(e, r_remove, behavior_version_equal);		// do not care about result here
		}

		return error;
	}
	if ((b & behavior_skip_lock) == 0) {
		pthread_rwlock_unlock(&this->_mutex_slot[mutex_index]);
	}
	r = result_none;

	return 0;
}

int storage_tch::remove(entry& e, result& r, int b) {
	log_info("remove (key=%s, expire=%ld, version=%llu)", e.key.c_str(), e.expire, e.version);
	int mutex_index = 0;

	if ((b & behavior_skip_lock) == 0) {
		mutex_index = e.get_key_hash_value(hash_algorithm_bitshift) % this->_mutex_slot_size;
	}

	try {
		if ((b & behavior_skip_lock) == 0) {
			pthread_rwlock_wrlock(&this->_mutex_slot[mutex_index]);
		}

		entry e_current;
		int e_current_exists = this->_get_header(e.key, e_current);
		if ((b & behavior_skip_version) == 0 && e.version != 0) {
			if (e.version <= e_current.version && ((b & behavior_version_equal) != 0 && e.version != e_current.version)) {
				log_info("specified version is older than (or equal to) current version -> skip removing (current=%u, specified=%u)", e_current.version, e.version);
				r = result_not_found;
				throw 0;
			}
		}

		if (e_current_exists < 0) {
			log_debug("data not found in database -> skip removing and updating header cache if we need", 0);
			if (e.version != 0) {
				this->_set_header_cache(e.key, e);
			}
			r = result_not_found;
			throw 0;
		}

		bool expired = false;
		if ((b & behavior_skip_timestamp) == 0 && e_current.expire > 0 && e_current.expire < stats_object->get_timestamp()) {
			log_info("data expired [expire=%d] -> result is NOT_FOUND but continue processing", e_current.expire);
			expired = true;
		}

		if (tchdbout(this->_db, e.key.c_str(), e.key.size()) == true) {
			r = expired ? result_not_found : result_deleted;
			log_debug("removed data (key=%s)", e.key.c_str());
		} else {
			int ecode = tchdbecode(this->_db);
			log_err("tchdbout() failed: %s (%d)", tchdberrmsg(ecode), ecode);
			throw -1;
		}
		if (expired == false) {
			if (e.version == 0) {
				e.version = e_current.version+1;
				log_debug("updating version (version=%llu)", e.version);
			}
			this->_set_header_cache(e.key, e);
		}
	} catch (int error) {
		if ((b & behavior_skip_lock) == 0) {
			pthread_rwlock_unlock(&this->_mutex_slot[mutex_index]);
		}
		return error;
	}
	if ((b & behavior_skip_lock) == 0) {
		pthread_rwlock_unlock(&this->_mutex_slot[mutex_index]);
	}

	return 0;
}

int storage_tch::truncate(int b) {
	log_info("truncating all data", 0);

	int i;
	if ((b & behavior_skip_lock) == 0) {
		for (i = 0; i < this->_mutex_slot_size; i++) {
			pthread_rwlock_wrlock(&this->_mutex_slot[i]);
		}
	}

	int r = 0;
	if (tchdbvanish(this->_db) == false) {
		int ecode = tchdbecode(this->_db);
		log_err("tchdbvanish() failed: %s (%d)", tchdberrmsg(ecode), ecode);
		r = -1;
	}

	this->_clear_header_cache();

	if ((b & behavior_skip_lock) == 0) {
		for (i = 0; i < this->_mutex_slot_size; i++) {
			pthread_rwlock_unlock(&this->_mutex_slot[i]);
		}
	}

	return r;
}

int storage_tch::iter_begin() {
	if (this->_iter_lock > 0) {
		return -1;
	}

	if (tchdbiterinit(this->_db) == false) {
		int ecode = tchdbecode(this->_db);
		log_err("tchdbiterinit() failed: %s (%d)", tchdberrmsg(ecode), ecode);
		return -1;
	}

	return 0;
}

int storage_tch::iter_next(string& key) {
	int len = 0;
	char* p = static_cast<char*>(tchdbiternext(this->_db, &len));
	if (p == NULL) {
		// end of iteration
		return -1;
	}
	key = p;
	free(p);

	return 0;
}

int storage_tch::iter_end() {
	this->_iter_lock = 0;

	return 0;
}

uint32_t storage_tch::count() {
	return tchdbrnum(this->_db);
}

uint64_t storage_tch::size() {
	return tchdbfsiz(this->_db);
}
// }}}

// {{{ protected methods
int storage_tch::_get_header(string key, entry& e) {
	log_debug("get header from database (key=%s)", e.key.c_str());
	uint8_t tmp_data[entry::header_size];
	int tmp_len = tchdbget3(this->_db, key.c_str(), key.size(), tmp_data, entry::header_size);
	if (tmp_len < 0) {
		// if not found, check data version cache
		log_debug("header not found in database -> checking cache", 0);
		this->_get_header_cache(key, e);
		return -1;
	}

	int offset = this->_unserialize_header(tmp_data, tmp_len, e);
	if (offset < 0) {
		log_err("failed to unserialize header (data is corrupted) (tmp_len=%d)", tmp_len);
		return -1;
	}

	log_debug("current header from database (key=%s, flag=%u, version=%llu, expire=%ld, size=%llu)", key.c_str(), e.flag, e.version, e.expire, e.size);

	return 0;
}
// }}}

// {{{ private methods
// }}}

}	// namespace flare
}	// namespace gree

// vim: foldmethod=marker tabstop=2 shiftwidth=2 autoindent
