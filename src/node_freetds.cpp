// Wrapper for the FreeTDS db-lib library
// -- Sugendran Ganess
//
// most of this is based on the excellent tutorials over at
// - http://syskall.com/how-to-write-your-own-native-nodejs-extension
// - https://www.cloudkick.com/blog/2010/aug/23/writing-nodejs-native-extensions/
// - http://nikhilm.bitbucket.org/articles/c_in_my_javascript/c_in_javascript_part_1.html
//
// In the end I decided to run with static functions
//  - it's just a wrapper so anything clever will need to be done in js
//
#include <v8.h>
#include <node.h>
#include <sqldb.h>
#include <sybdb.h>
#include <stdio.h>
#include <string.h>

// handy helpers for extracting arguments
#define REQ_FUN_ARG(I, VAR)                                             \
  if (args.Length() <= (I) || !args[I]->IsFunction())                   \
    return ThrowException(Exception::TypeError(                         \
                  String::New("Argument " #I " must be a function")));  \
  Local<Function> VAR = Local<Function>::Cast(args[I]);

#define REQ_OBJ_ARG(I, VAR)                                             \
  if (args.Length() <= (I) || !args[I]->IsObject())                     \
    return ThrowException(Exception::TypeError(                         \
                  String::New("Argument " #I " must be an object")));   \
  Local<Object> VAR = Local<Object>::Cast(args[I]);

#define REQ_STR_ARG(I, VAR)                                             \
  if (args.Length() <= (I) || !args[I]->IsString())                     \
    return ThrowException(Exception::TypeError(                         \
                  String::New("Argument " #I " must be an object")));   \
  Local<String> VAR = Local<String>::Cast(args[I]);

#define REQ_DBCONN_ARG(I) DBPROCESS *dbconn = (DBPROCESS*) v8::External::Unwrap(args[I]);

using namespace v8;
using namespace node;


namespace FreeTDS {

    struct data_callback_t {
      DBPROCESS *dbconn;
      Persistent<Function> callback;
    };

    struct COL {
      Local<String> name;
      void *buffer;
      int type, size, status;
    };

    static v8::Handle<Value> Version(const Arguments& args) {
      v8::HandleScope scope;
      return v8::String::New(rcsid_sqldb_h);
    }

    // assumes {userId: 'user name', password: 'password', server: 'server', database: 'database' }
    // returns a database reference
    static v8::Handle<Value> Login(const Arguments& args) {
      v8::HandleScope scope;
      LOGINREC *login;
      DBPROCESS *dbconn;

      REQ_OBJ_ARG(0, connArgs);

      /* Allocate a login params structure */
      if ((login = dblogin()) == FAIL) {
        return v8::ThrowException(v8::Exception::Error(v8::String::New("FreeTDS cannot initialise dblogin() structure.")));
      }

      // fill out the login params
      v8::Local<v8::String> userIdKey = v8::String::New("userId");
      if(connArgs->Has(userIdKey)){
        String::AsciiValue userId(connArgs->Get(userIdKey));
        DBSETLUSER(login, *userId);
      }

      v8::Local<v8::String> passwordKey = v8::String::New("password");
      if(connArgs->Has(passwordKey)){
        String::AsciiValue password(connArgs->Get(passwordKey));
        DBSETLPWD(login, *password);
      }

      // set the application name to node-freetds to help with profiling etc
      DBSETLAPP(login, "node-freetds");      

      v8::Local<v8::String> serverKey = v8::String::New("server");
      if(!connArgs->Has(serverKey)){
        dbloginfree(login);
        return v8::ThrowException(v8::Exception::Error(v8::String::New("The server name was not provided")));
      }
      String::AsciiValue serverName(connArgs->Get(serverKey));
      
      /* Now connect to the DB Server */
      if ((dbconn = dbopen(login, *serverName)) == NULL) {
        dbloginfree(login);
        return v8::ThrowException(v8::Exception::Error(v8::String::New("FreeTDS cannot initialise dblogin() structure.")));
      }

      v8::Local<v8::String> dbKey = v8::String::New("database");
      if(connArgs->Has(dbKey)){
        String::AsciiValue database(connArgs->Get(dbKey));
        /* Now switch to the correct database */
        if ((dbuse(dbconn, *database)) == FAIL) {
          dbloginfree(login);
          return v8::ThrowException(v8::Exception::Error(v8::String::New("FreeTDS could not switch to the database")));
        }
      }

      // free the login struct because we don't need it anymore
      dbloginfree(login);

      // wrap the dbconn so that we can persist it from the JS side
      return v8::External::Wrap(dbconn);
    }

    // one arg - logout
    static v8::Handle<Value> Logout(const Arguments& args) {
      v8::HandleScope scope;
      REQ_DBCONN_ARG(0);
      dbfreebuf(dbconn);
      dbclose(dbconn);
      return v8::Undefined();
    }

    static v8::Handle<Value> Cleanup(const Arguments& args) {
      v8::HandleScope scope;
      dbexit();
      return v8::Undefined();
    }

    static void waitForDataResponse(eio_req *req) {
      data_callback_t *callbackData = (data_callback_t *) req->data;
      //dbsqlok
      req->result = dbsqlok(callbackData->dbconn);
    }

    static int onDataResponse(eio_req *req) {
      v8::HandleScope scope;
      data_callback_t *callbackData = (data_callback_t *) req->data;
      if(req->result == FAIL){
        Local<Value> argv[1];
        argv[0] = v8::Exception::Error(v8::String::New("An error occured executing that statement"));
        callbackData->callback->Call(Context::GetCurrent()->Global(), 1, argv);
      }

      uint32_t rownum = 0;
      bool err = false;
      COL *columns, *pcol;
      int ncols = 0;
      v8::Local<v8::Array> results = v8::Array::New();
      while(dbresults(callbackData->dbconn) != NO_MORE_RESULTS){
        ncols = dbnumcols(callbackData->dbconn);
        columns = (COL *) calloc(ncols, sizeof(struct COL));
        for (pcol = columns; pcol - columns < ncols; pcol++) {
          int i = pcol - columns + 1;
          pcol->name = v8::String::New(dbcolname(callbackData->dbconn, i));
          pcol->type = dbcoltype(callbackData->dbconn, i);
          pcol->size = dbcollen(callbackData->dbconn, i);

          if (SYBCHAR != pcol->type) {      
            pcol->size = dbwillconvert(pcol->type, SYBCHAR);
          }
          //todo: work out if I'm leaking
          if((pcol->buffer = (void *) malloc(pcol->size + 1)) == NULL) {
            err = true;
            break;
          }
        }
        if(err){
          for (pcol = columns; pcol - columns < ncols; pcol++) {
            free(pcol->buffer);
            free(columns);
          }
          Local<Value> argv[1];
          argv[0] = v8::Exception::Error(v8::String::New("Could not allocate memory for columns"));
          callbackData->callback->Call(Context::GetCurrent()->Global(), 1, argv);
          return 0;
        }
        for (pcol = columns; pcol - columns < ncols; pcol++) {
          int i = pcol - columns + 1;
          int binding = NTBSTRINGBIND;
          switch(pcol->type){
            case TINYBIND:
            case SMALLBIND:
            case INTBIND:
            case FLT8BIND:
            case REALBIND:
            case SMALLDATETIMEBIND:
            case MONEYBIND:
            case SMALLMONEYBIND:
            case BINARYBIND:
            case BITBIND:
            case NUMERICBIND:
            case DECIMALBIND:
            case BIGINTBIND:
              // all numbers in JS are doubles
              binding = REALBIND;
              break;
          }
          if(dbbind(callbackData->dbconn, i, binding,  pcol->size + 1, (BYTE*)pcol->buffer) == FAIL){
            err = true;
          }else if(dbnullbind(callbackData->dbconn, i, &pcol->status) == FAIL){
            err = true;
          }
        }
        if(err){
          for (pcol = columns; pcol - columns < ncols; pcol++) {
            free(pcol->buffer);
            free(columns);
          }
          Local<Value> argv[1];
          argv[0] = v8::Exception::Error(v8::String::New("Could not allocate memory for columns"));
          callbackData->callback->Call(Context::GetCurrent()->Global(), 1, argv);
          return 0;
        }
        int row_code;
        while ((row_code = dbnextrow(callbackData->dbconn)) != NO_MORE_ROWS){ 
          if(row_code == REG_ROW) {
            v8::Local<v8::Object> tuple = v8::Object::New();
            for (pcol = columns; pcol - columns < ncols; pcol++) {
              if(pcol->status == -1){
                tuple->Set(pcol->name, v8::Null());
                continue;
              }
              switch(pcol->type){
                case CHARBIND:
                case STRINGBIND:
                case NTBSTRINGBIND:
                case VARYCHARBIND:
                case VARYBINBIND:
                  tuple->Set(pcol->name, v8::String::New((char*) pcol->buffer));
                  break;
                case TINYBIND:
                case SMALLBIND:
                case INTBIND:
                case FLT8BIND:
                case REALBIND:
                case DATETIMEBIND:
                case SMALLDATETIMEBIND:
                case MONEYBIND:
                case SMALLMONEYBIND:
                case BINARYBIND:
                case BITBIND:
                case NUMERICBIND:
                case DECIMALBIND:
                case BIGINTBIND:
                  DBREAL val;
                  memcpy(&val, pcol->buffer, pcol->size);
                  tuple->Set(pcol->name, v8::Number::New((double)val));
                  break;
              }
            }
            results->Set(rownum++, tuple);
          }
        }
      }

      v8::Local<v8::Value> argv[2] = { Local<Value>::New(Null()), results };
      callbackData->callback->Call(Context::GetCurrent()->Global(), 2, argv);

      callbackData->callback.Dispose();
      delete callbackData;

      for (pcol = columns; pcol - columns < ncols; pcol++) {
        free(pcol->buffer);
        free(columns);
      }

      return 0;
    }

    // db, statement, callback
    static v8::Handle<Value> ExecuteStatement(const Arguments& args) {
      v8::HandleScope scope;
      REQ_DBCONN_ARG(0);
      REQ_STR_ARG(1, stmt);
      REQ_FUN_ARG(2, cb);

      v8::String::Utf8Value statement(stmt);

      if(dbcmd(dbconn, *statement) == FAIL){
        return v8::ThrowException(v8::Exception::Error(v8::String::New("FreeTDS could allocate enough memory for the statement")));
      }

      data_callback_t *callbackData = new data_callback_t();
      callbackData->dbconn = dbconn;
      callbackData->callback = Persistent<Function>::New(cb);

      if(dbsqlsend(dbconn) == FAIL){
        return v8::ThrowException(v8::Exception::Error(v8::String::New("FreeTDS could not send the statement")));
      }
      
      eio_custom(waitForDataResponse, EIO_PRI_DEFAULT, onDataResponse, callbackData);
      return v8::Undefined();
    }

}

extern "C"
void init( Handle<Object> target ) {
  HandleScope scope;
  
  // start up db-lib
  if(dbinit() == FAIL){
    v8::ThrowException(v8::Exception::Error(v8::String::New("FreeTDS cannot allocate an socket pointers")));
    return;
  }

  NODE_SET_METHOD(target, "version", FreeTDS::Version);
  NODE_SET_METHOD(target, "login", FreeTDS::Login);
  NODE_SET_METHOD(target, "logout", FreeTDS::Logout);
  NODE_SET_METHOD(target, "cleanup", FreeTDS::Cleanup);
  NODE_SET_METHOD(target, "executeSql", FreeTDS::ExecuteStatement);
}