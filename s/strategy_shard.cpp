// strategy_sharded.cpp

#include "stdafx.h"
#include "request.h"
#include "shard.h"
#include "../client/connpool.h"
#include "../db/commands.h"

namespace mongo {
    
    class ServerAndQuery {
    public:
        ServerAndQuery( const string& server , BSONObj extra = emptyObj , BSONObj orderObject = emptyObj ) : 
            _server( server ) , _extra( extra ) , _orderObject( orderObject ){
        }

        bool operator<( const ServerAndQuery& other ) const{
            if ( ! _orderObject.isEmpty() )
                return _orderObject.woCompare( other._orderObject ) < 0;
            
            if ( _server < other._server )
                return true;
            if ( other._server > _server )
                return false;
            return _extra.woCompare( other._extra ) < 0;
        }

        string _server;
        BSONObj _extra;
        BSONObj _orderObject;
    };
    
    class SerialServerShardedCursor : public ShardedCursor {
    public:
        SerialServerShardedCursor( set<ServerAndQuery> servers , QueryMessage& q , int sortOrder=0) : ShardedCursor( q ){
            for ( set<ServerAndQuery>::iterator i = servers.begin(); i!=servers.end(); i++ )
                _servers.push_back( *i );

            if ( sortOrder > 0 )
                sort( _servers.begin() , _servers.end() );
            else if ( sortOrder < 0 )
                sort( _servers.rbegin() , _servers.rend() );
                    
            _serverIndex = 0;
        }
        
        virtual bool more(){
            if ( _current.get() && _current->more() )
                return true;

            if ( _serverIndex >= _servers.size() )
                return false;

            ServerAndQuery& sq = _servers[_serverIndex++];
            _current = query( sq._server , 0 , sq._extra );
            return _current->more();
        }

        virtual BSONObj next(){
            uassert( "no more items" , more() );
            return _current->next();
        }
        
    private:
        vector<ServerAndQuery> _servers;
        unsigned _serverIndex;
        
        auto_ptr<DBClientCursor> _current;
    };
    
    class ShardStrategy : public Strategy {

        virtual void queryOp( Request& r ){
            QueryMessage q( r.d() );
            
            log(3) << "shard query: " << q.ns << "  " << q.query << endl;

            if ( q.ntoreturn == 1 && strstr(q.ns, ".$cmd") )
                throw UserException( "something is wrong, shouldn't see a command here" );

            ShardInfo * info = r.getShardInfo();
            assert( info );
            
            Query query( q.query );

            vector<Shard*> shards;
            info->getShardsForQuery( shards , query.getFilter()  );
            
            set<ServerAndQuery> servers;
            map<string,int> serverCounts;
            for ( vector<Shard*>::iterator i = shards.begin(); i != shards.end(); i++ ){
                servers.insert( (*i)->getServer() );
                int& num = serverCounts[(*i)->getServer()];
                num++;
            }
            
            if ( servers.size() == 1 ){
                doQuery( r , servers.begin()->_server );
                return;
            }
            
            SerialServerShardedCursor * cursor = 0;
            
            BSONObj sort = query.getSort();
            
            if ( sort.isEmpty() ){
                // 1. no sort, can just hit them in serial
                cursor = new SerialServerShardedCursor( servers , q );
            }
            else {
                int shardKeyOrder = info->getShardKey().isMatchAndOrder( sort );
                if ( shardKeyOrder ){
                    // 2. sort on shard key, can do in serial intelligently
                    set<ServerAndQuery> buckets;
                    for ( vector<Shard*>::iterator i = shards.begin(); i != shards.end(); i++ ){
                        Shard * s = *i;
                        BSONObj extra = emptyObj;
                        if ( serverCounts[s->getServer()] > 1 ){
                            BSONObjBuilder b;
                            s->getFilter( b );
                            extra = b.obj();
                        }
                        buckets.insert( ServerAndQuery( s->getServer() , extra , s->getMin() ) );
                    }
                    cursor = new SerialServerShardedCursor( buckets , q , shardKeyOrder );
                }
                else {
                    // 3. sort on non-sharded key, pull back a portion from each server and iterate slowly
                    throw UserException( "sorting and sharding doesn't work on a arbitrary field yet" );
                }
            }
            
            assert( cursor );
            cursor->sendNextBatch( r );
        }
        
        virtual void getMore( Request& r ){
            throw UserException( "shard getMore doesn't work yet" );
        }
        
        virtual void writeOp( int op , Request& r ){
            
            const char *ns = r.getns();
            log(3) << "write: " << ns << endl;
            
            DbMessage& d = r.d();
            ShardInfo * info = r.getShardInfo();
            assert( info );
            
            if ( op == dbInsert ){
                while ( d.moreJSObjs() ){
                    BSONObj o = d.nextJsObj();
                    if ( ! info->hasShardKey( o ) ){
                        log() << "tried to insert object without shard key: " << ns << "  " << o << endl;
                        throw UserException( "tried to insert object without shard key" );
                    }
                    
                    Shard& s = info->findShard( o );
                    log(4) << "  server:" << s.getServer() << " " << o << endl;
                    insert( s.getServer() , ns , o );
                }
            }
            else if ( op == dbUpdate ){
                throw UserException( "can't do update yet on sharded collection" );
            }
            else {
                log() << "sharding can't do write op: " << op << endl;
                throw UserException( "can't do this write op on sharded collection" );
            }
            
        }
    };
    
    Strategy * SHARDED = new ShardStrategy();
}
