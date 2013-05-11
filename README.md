libriak-c
=========

An curl based RIAK client library written in ANSI C 

Usage example:
  int main(int argc, char** argv) {
  
      _riak_optional_p op;
      op.content_type = "text/plain";
      op.link = NULL;
      op.x_riak_index = NULL;
      op.x_riak_meta = NULL;
      op.x_riak_vclock = NULL;
      op.autokey = false;
      op.returnbody = true;
      op.x_riak_clid = NULL;
  
      //
      RIAK_HNDL rh = RiakInit("", "", false); /* host ip, port , use ssl */
      bool ret = RiakPing(rh);
      printf("Riak %s\n", (ret) ? "OK" : "Failed");
      //
      char *body = RiakGetKey(rh, "test", "doc");
      printf("%s -> %d\n", (NULL != body) ? body : "", (NULL != body) ? strlen(body) : 0);
      RiakFreeMem(get_key_op_result.body);
      //
      
      char **list = RiakListBuckets(rh);
  
      while (*list) {
          printf("%s\n", *list);
          ++list;
      }
  
      char **klist = RiakListKeys(rh, "rekon", true);
  
      while (*klist != NULL) {
          printf("%s\n", *klist);
          ++klist;
      }
   
      
      RiakSetKey(rh, "test", "hafo", "bobo", &op);
      //
      RiakDeleteKey(rh, "test", "hafo", &op);
      //
      int r = RiakResetBucketProps(rh, "test");
  
      RiakClose(rh);
      return (EXIT_SUCCESS);
      }
  
  Known bugs:
  
    RiakListKeys - does not parse keys while keys=stream
    RiakGetBucketProps - not implemented yet
    
Some features are in development stage yet, plz , do not rely on them.
Any suggestion, feature, bug or comment will be appreciated
