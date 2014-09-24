// Copyright 2010-2014 RethinkDB, all rights reserved.
#ifndef RDB_PROTOCOL_CHANGEFEED_HPP_
#define RDB_PROTOCOL_CHANGEFEED_HPP_

#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include <utility>

#include "errors.hpp"
#include <boost/variant.hpp>

#include "concurrency/rwlock.hpp"
#include "containers/counted.hpp"
#include "containers/scoped.hpp"
#include "btree/keys.hpp"
#include "protocol_api.hpp"
#include "rdb_protocol/counted_term.hpp"
#include "rdb_protocol/datum.hpp"
#include "rdb_protocol/shards.hpp"
#include "region/region.hpp"
#include "repli_timestamp.hpp"
#include "rpc/connectivity/peer_id.hpp"
#include "rpc/mailbox/typed.hpp"
#include "rpc/serialize_macros.hpp"

class auto_drainer_t;
class mailbox_manager_t;
class namespace_interface_access_t;
class superblock_t;
struct rdb_modification_report_t;

namespace ql {

class base_exc_t;
class batcher_t;
class datum_stream_t;
class env_t;
class table_t;

namespace changefeed {

typedef std::function<stream_t(superblock_t *superblock,
                               const datum_range_t &active_range,
                               const std::string &table_name,
                               const std::string &sindex,
                               size_t n)> read_func_t;

typedef std::function<stream_t(const datum_range_t &active_range,
                               const std::string &table_name,
                               const std::string &sindex,
                               size_t n)> pure_read_func_t;

// RSI: should this ever be used?
static stream_t no_read_func_needed_f(superblock_t *,
                                      const datum_range_t &,
                                      const std::string &,
                                      const boost::optional<std::string> &,
                                      size_t) {
    guarantee(false);
    unreachable();
}
static read_func_t no_read_func_needed = &no_read_func_needed_f;

struct msg_t {
    struct limit_start_t {
        uuid_u sub;
        // RSI: send this!
        std::vector<std::pair<datum_t, datum_t> > start_data;
        RDB_DECLARE_ME_SERIALIZABLE;
    };
    struct limit_change_t {
        uuid_u sub;
        boost::optional<datum_t> old_key;
        boost::optional<std::pair<datum_t, datum_t> > new_val;
        RDB_DECLARE_ME_SERIALIZABLE;
    };
    struct change_t {
        change_t() { };
        change_t(datum_t _old_val, datum_t _new_val)
            : old_val(std::move(_old_val)), new_val(std::move(_new_val)) { }
        ~change_t() { }
        datum_t old_val, new_val;
        RDB_DECLARE_ME_SERIALIZABLE;
    };
    struct stop_t { };

    msg_t() { }
    msg_t(msg_t &&msg) : op(std::move(msg.op)) { }
    template<class T>
    explicit msg_t(T &&_op) : op(std::move(_op)) { }

    // We need to define the copy constructor.  GCC 4.4 doesn't let us use `=
    // default`, and SRH is uncomfortable violating the rule of 3, so we define
    // the destructor and assignment operator as well.
    msg_t(const msg_t &msg) : op(msg.op) { }
    ~msg_t() { }
    const msg_t &operator=(const msg_t &msg) {
        op = msg.op;
        return *this;
    }

    // Starts with STOP to avoid doing work for default initialization.
    boost::variant<stop_t, change_t, limit_start_t, limit_change_t> op;
};

RDB_SERIALIZE_OUTSIDE(msg_t::limit_start_t);
RDB_SERIALIZE_OUTSIDE(msg_t::limit_change_t);
RDB_SERIALIZE_OUTSIDE(msg_t::change_t);
RDB_DECLARE_SERIALIZABLE(msg_t::stop_t);
RDB_DECLARE_SERIALIZABLE(msg_t);

class feed_t;
struct stamped_msg_t;

typedef mailbox_addr_t<void(stamped_msg_t)> client_addr_t;

struct keyspec_t {
    struct range_t {
        range_t() { }
        range_t(datum_range_t _range) : range(std::move(_range)) { }
        datum_range_t range;
    };
    struct limit_t {
        limit_t() { }
        limit_t(datum_range_t _range, std::string _sindex,
                sorting_t _sorting, size_t _limit)
            : range(std::move(_range)), sindex(std::move(_sindex)),
              sorting(_sorting), limit(_limit) { }
        datum_range_t range;
        std::string sindex;
        sorting_t sorting;
        size_t limit;
        RDB_DECLARE_ME_SERIALIZABLE;
    };
    struct point_t {
        point_t() { }
        explicit point_t(datum_t _key) : key(std::move(_key)) { }
        datum_t key;
    };

    keyspec_t(keyspec_t &&keyspec) = default;
    explicit keyspec_t(range_t &&range) : spec(std::move(range)) { }
    explicit keyspec_t(limit_t &&limit) : spec(std::move(limit)) { }
    explicit keyspec_t(point_t &&point) : spec(std::move(point)) { }

    // This needs to be copyable and assignable because it goes inside a
    // `changefeed_stamp_t`, which goes inside a variant.
    keyspec_t(const keyspec_t &keyspec) = default;
    keyspec_t &operator=(const keyspec_t &) = default;

    boost::variant<range_t, limit_t, point_t> spec;
private:
    keyspec_t() { }
};
region_t keyspec_to_region(const keyspec_t &keyspec);

RDB_SERIALIZE_OUTSIDE(keyspec_t::limit_t);
RDB_DECLARE_SERIALIZABLE(keyspec_t::limit_t);
RDB_DECLARE_SERIALIZABLE(keyspec_t::point_t);
RDB_DECLARE_SERIALIZABLE(keyspec_t);

// The `client_t` exists on the machine handling the changefeed query, in the
// `rdb_context_t`.  When a query subscribes to the changes on a table, it
// should call `new_feed`.  The `client_t` will give it back a stream of rows.
// The `client_t` does this by maintaining an internal map from table UUIDs to
// `feed_t`s.  (It does this so that there is at most one `feed_t` per <table,
// client> pair, to prevent redundant cluster messages.)  The actual logic for
// subscribing to a changefeed server and distributing writes to streams can be
// found in the `feed_t` class.
class client_t : public home_thread_mixin_t {
public:
    typedef client_addr_t addr_t;
    client_t(
        mailbox_manager_t *_manager,
        const std::function<
            namespace_interface_access_t(
                const namespace_id_t &,
                signal_t *)
            > &_namespace_source
        );
    ~client_t();
    // Throws QL exceptions.
    counted_t<datum_stream_t> new_feed(
        env_t *env,
        const namespace_id_t &table,
        const protob_t<const Backtrace> &bt,
        const std::string &table_name,
        const std::string &pkey,
        const keyspec_t &keyspec);
    void maybe_remove_feed(const namespace_id_t &uuid);
    scoped_ptr_t<feed_t> detach_feed(const namespace_id_t &uuid);
private:
    friend class subscription_t;
    mailbox_manager_t *const manager;
    std::function<
        namespace_interface_access_t(
            const namespace_id_t &,
            signal_t *)
        > const namespace_source;
    std::map<namespace_id_t, scoped_ptr_t<feed_t> > feeds;
    // This lock manages access to the `feeds` map.  The `feeds` map needs to be
    // read whenever `new_feed` is called, and needs to be written to whenever
    // `new_feed` is called with a table not already in the `feeds` map, or
    // whenever `maybe_remove_feed` or `detach_feed` is called.
    //
    // This lock is held for a long time when `new_feed` is called with a table
    // not already in the `feeds` map (in fact, it's held long enough to do a
    // cluster read).  This should only be a problem if the number of tables
    // (*not* the number of feeds) is large relative to read throughput, because
    // otherwise most of the calls to `new_feed` that block will see the table
    // as soon as they're woken up and won't have to do a second read.
    rwlock_t feeds_lock;
    auto_drainer_t drainer;
};

typedef mailbox_addr_t<void(client_addr_t)> server_addr_t;

class server_t;
class limit_manager_t {
public:
    limit_manager_t(
        std::string _table,
        std::string _sindex,
        server_t *_parent,
        client_t::addr_t _parent_client,
                uuid_u _uuid,
        keyspec_t::limit_t _spec,
        std::multimap<datum_t, datum_t, decltype(std::less<const datum_t &>())>
            &&start_data)
        : table(std::move(_table)),
          sindex(std::move(_sindex)),
          parent(_parent),
          parent_client(std::move(_parent_client)),
          uuid(std::move(_uuid)),
          spec(std::move(_spec)),
          data(std::move(start_data)) { }

    // Make sure you have a lock (e.g. the lock provided by `foreach_limit`)
    // before calling these.
    void del(datum_t key);
    void add(datum_t key, datum_t val);
    void commit(const pure_read_func_t &read_func);
    bool operator<(const limit_manager_t &other) {
        return (sindex < other.sindex)
            ? true
            : ((sindex > other.sindex)
               ? false
               : uuid < other.uuid);
    }

    const std::string table, sindex;
private:
    server_t *parent;
    client_t::addr_t parent_client;

    uuid_u uuid;
    keyspec_t::limit_t spec;
    // RSI: sorting
    std::multimap<datum_t, datum_t, decltype(std::less<const datum_t &>())> data;

    std::vector<std::pair<datum_t, datum_t> > added;
    std::vector<datum_t> deleted;
public:
    rwlock_t lock;
    auto_drainer_t drainer;
};

// There is one `server_t` per `store_t`, and it is used to send changes that
// occur on that `store_t` to any subscribed `feed_t`s contained in a
// `client_t`.
class server_t {
public:
    typedef server_addr_t addr_t;
    explicit server_t(mailbox_manager_t *_manager);
    ~server_t();
    void add_client(const client_t::addr_t &addr, region_t region);
    void add_limit_client(
        const client_t::addr_t &addr,
        const region_t &region,
        const uuid_u &client_uuid,
        const keyspec_t::limit_t &spec);
    // `key` should be non-NULL if there is a key associated with the message.
    void send_all(const msg_t &msg, const store_key_t &key);
    void stop_all();
    addr_t get_stop_addr();
    uint64_t get_stamp(const client_t::addr_t &addr);
    uuid_u get_uuid();
    // `f` will be called with a read lock on `clients` and a write lock on the
    // limit manager.
    void foreach_limit(const std::string &s, std::function<void(limit_manager_t *)> f);
private:
    friend class limit_manager_t;
    void stop_mailbox_cb(client_t::addr_t addr);
    void add_client_cb(signal_t *stopped, client_t::addr_t addr);

    // The UUID of the server, used so that `feed_t`s can enforce on ordering on
    // changefeed messages on a per-server basis (and drop changefeed messages
    // from before their own creation timestamp on a per-server basis).
    const uuid_u uuid;
    mailbox_manager_t *const manager;

    struct client_info_t {
        scoped_ptr_t<cond_t> cond;
        uint64_t stamp;
        std::vector<region_t> regions;
        std::map<std::string, std::vector<limit_manager_t> > limit_clients;
    };
    std::map<client_t::addr_t, client_info_t> clients;

    void send_one_with_lock(const auto_drainer_t::lock_t &lock,
                            std::pair<const client_t::addr_t, client_info_t> *client,
                            msg_t msg);

    // Controls access to `clients`.  A `server_t` needs to read `clients` when:
    // * `send_all` is called
    // * `get_stamp` is called
    // And needs to write to clients when:
    // * `add_client` is called
    // * `clear` is called
    // * A message is received at `stop_mailbox` unsubscribing a client
    // A lock is needed because e.g. `send_all` calls `send`, which can block,
    // while looping over `clients`, and we need to make sure the map doesn't
    // change under it.
    rwlock_t clients_lock;

    auto_drainer_t drainer;
    // Clients send a message to this mailbox with their address when they want
    // to unsubscribe.  The callback of this mailbox acquires the drainer, so it
    // has to be destroyed first.
    mailbox_t<void(client_t::addr_t)> stop_mailbox;
};

} // namespace changefeed
} // namespace ql

#endif // RDB_PROTOCOL_CHANGEFEED_HPP_

