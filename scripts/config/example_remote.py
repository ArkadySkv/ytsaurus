#!/usr/bin/python
from cfglib.ytremote import *
import cfglib.opts as opts

Logging = {
    'writers' : {
        'raw' :
        {
            'type' : 'raw',
            'file_name' : "/yt/%(log_disk)s/data/logs/%(debug_log_path)s"
        },
        'file' :
        {
            'type' : "file",
            'file_name' : "/yt/%(log_disk)s/data/logs/%(log_path)s",
            'pattern' : "$(datetime) $(level) $(category) $(message)"
        }
    },
    'rules' : [
        { 
            'categories' : [ "*" ],
            'min_level' : "debug",
            'writers' : [ "raw" ]
        },
        {
            'categories' : [ "*" ],
            'min_level' : "info",
            'writers' : [ "file" ]
        }
    ]
}


MasterAddresses = opts.limit_iter('--masters', ['meta01-00%de.yt.yandex.net:9000' % i for i in xrange(1, 4)])

class Base(AggrBase):
    path = opts.get_string('--name', 'control')
    base_dir = '/yt/disk1/data'
    libs = [
        '/home/yt/build/lib/libyajl.so.2',
        '/home/yt/build/lib/libytext-json.so',
        '/home/yt/build/lib/libytext-uv.so.0.6'
    ]
    
    def get_log(cls, fd):
        print >>fd, shebang
        print >>fd, 'rsync %s:%s %s' % (cls.host, cls.config['logging']['writers']['file']['file_name'], cls.local_dir)
    
class Server(Base, RemoteServer):
    bin_path = '/home/yt/build/bin/ytserver'

    def do_clean(cls, fd):
        print >>fd, shebang
        print >>fd, 'rm -rf %s' % cls.log_path
        print >>fd, 'rm -rf %s' % cls.debug_log_path
    
class Master(Server):
    base_dir = '/yt/disk2/data'
    address = Subclass(MasterAddresses)
    params = Template('--master --config %(config_path)s')

    log_disk = 'disk1'
    log_path = Template("master-%(__name__)s.log")
    debug_log_path = Template("master-%(__name__)s.debug.log")

    config = Template({
        'address_resolver' : {
            'enable_ipv6' : 'false'
        },
        'meta_state' : {
            'leader_committer' : {
                'max_batch_delay': 50
            },
            'cell' : {
                'addresses' : MasterAddresses,
                'rpc_port' : 9000
            },
            'snapshots' : {
                'path' : '/yt/disk1/data/snapshots',
            },
            'changelogs' : {
                'path' : '/yt/disk2/data/changelogs',
            },
            'max_changes_between_snapshots' : 1000000,
            'changelog_downloader' : {
                'records_per_request' : 10240
            },
        },
        'chunks' : {
            'registered_node_timeout' : 180000,
            'chunk_replicator' : {
                'max_lost_chunk_fraction' : 0.01
            }
        },
        'monitoring_port' : 10000, 
        'logging' : Logging
    })
    
    def do_clean(cls, fd):
        print >>fd, shebang
        print >>fd, 'rm -rf %s' % cls.log_path
        print >>fd, 'rm -rf %s' % cls.debug_log_path
        print >>fd, 'rm -rf %s/*' % cls.config['meta_state']['snapshots']['path']
        print >>fd, 'rm -rf %s/*' % cls.config['meta_state']['changelogs']['path']
        
class Scheduler(Server):
    base_dir = '/yt/disk2/data'
    address = MasterAddresses[0]
    params = Template('--scheduler --config %(config_path)s')

    log_disk = 'disk1'
    log_path = "scheduler.log"
    debug_log_path = "scheduler.debug.log"

    config = Template({
        'address_resolver' : {
            'enable_ipv6' : 'false'
        },
        'masters' : {
            'addresses' : MasterAddresses
        },
        'scheduler' : {   
            'strategy' : 'fair_share',
            'snapshot_temp_path' : '/yt/disk1/data/temp'
        },
        'rpc_port' : 9001,
        'monitoring_port' : 10001, 
        'logging' : Logging,
        "address_resolver": { "enable_ipv6": "false" }
    })

CleanCache = FileDescr('clean_cache', ('aggregate', 'exec'))
DoCleanCache = FileDescr('do_clean_cache', ('remote', 'exec'))

class Holder(Server):
    files = Server.files + [CleanCache, DoCleanCache]

    #groupid = Subclass(xrange(5))
    nodeid = Subclass(xrange(49))

    log_disk = 'disk1'
    log_path = Template("node-%(nodeid)d.log")
    debug_log_path = Template("node-%(nodeid)d.debug.log")
    
    @propmethod
    def host(cls):
        return 'n01-0%03de.yt.yandex.net' % (1 + cls.nodeid)
    
    params = Template('--node --config %(config_path)s')

    proxyLogging = deepcopy(Logging)
    proxyLogging['writers']['raw']['file_name'] = 'job_proxy.debug.log'
    proxyLogging['writers']['file']['file_name'] = 'job_proxy.log'

    storeQuota = 1700 * 1024 * 1024 * 1024 # the actual limit is ~1740
    cacheQuota = 1 * 1024 * 1024 * 1024
    config = Template({ 
        'address_resolver' : {
            'enable_ipv6' : 'false'
        },
        'masters' : {
            'addresses' : MasterAddresses,
            'rpc_timeout' : 20000
        },
        'data_node' : {
            'store_locations' : [
                { 'path' : '/yt/disk1/data/chunk_store', 'quota' : storeQuota },
                { 'path' : '/yt/disk2/data/chunk_store', 'quota' : storeQuota },
                { 'path' : '/yt/disk3/data/chunk_store', 'quota' : storeQuota },
                { 'path' : '/yt/disk4/data/chunk_store', 'quota' : storeQuota }
            ],
            'cache_location' : {
                'path' : '/yt/disk2/data/chunk_cache', 'quota' : cacheQuota
            },
            'cache_remote_reader' : { 
                'publish_peer' : 'true'
            },
            'full_heartbeat_timeout' : 180000,
            'max_cached_blocks_size' : 4 * 1024 * 1024 * 1024,
            'max_cached_readers' : 256,
            'response_throttling_size' : 500 * 1024 * 1024
        },
        'exec_agent' : {
            'environment_manager' : {
                'environments' : {
                    'default' : {
                        'type' : 'unsafe'
                    }
                }
            },
            'job_controller': {
                'resource_limits' : {
                    'user_slots' : 24,
                    'cpu' : 22,
                    'memory' : 42 * 1024 * 1024 * 1024
                }
            },
            'slot_manager' : {
                'slot_location' : '%(work_dir)s/slots'
            },
            'job_proxy_logging' : proxyLogging,
        },
        'rpc_port' : 9002,
        'monitoring_port' : 10002, 
        'logging' : Logging
    })
    
    def do_clean(cls, fd):
        print >>fd, shebang
        print >>fd, 'rm -rf %s' % cls.log_path
        print >>fd, 'rm -rf %s' % cls.debug_log_path
        for location in cls.config['data_node']['store_locations']:
            print >>fd, 'rm -rf %s' % location['path']
        print >>fd, 'rm -rf %s' % cls.config['data_node']['cache_location']['path']

    def do_clean_cache(cls, fd):
        print >>fd, shebang
        print >>fd, 'rm -rf %s' % cls.config['data_node']['cache_location']['path']


configure(Base)
    
