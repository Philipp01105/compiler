# Additional acceptance cases for plans/stdlib-language-gaps-next.md.
set(growing_ring [=[
struct Reader<'a> { var source:&'a i32; }
struct Ring<'a> {
 var storage:*Reader<'a>; var head:usize; var slots:usize; var count:usize;
 func push(value:Reader<'a>)->void { core.initialize(&storage[(head+count)%slots],value); count+=1; }
 func grow(size:usize)->void {
  var fresh=core.core_alloc(size*sizeof(Reader)).(*Reader<'a>);
  if(fresh==core.null<Reader<'a>>()) { return; }
  for(var cursor:usize=0;cursor<count;cursor+=1) { core.initialize(&fresh[cursor],take(storage[(head+cursor)%slots])); }
  core.core_release(storage.(*u8)); storage=fresh; slots=size; head=0;
 }
 func pop()->Reader<'a> { count-=1; return take(storage[(head+count)%slots]); }
 func clear()->void { while(count>0) { core.destroy(&storage[head]); head=(head+1)%slots; count-=1; } }
 destructor { core.core_release(storage.(*u8)); }
}
]=])
set(growing_ring_setup "var left:i32=1; var right:i32=2; var ring=Ring{storage:core.core_alloc(3*sizeof(Reader)).(*Reader),head:2,slots:3,count:0}; ring.push(Reader{source:&left}); ring.push(Reader{source:&right}); if(ring.count!=2) { return; } ring.grow(4);")
reject(ring_growth_remaining "${growing_ring} func main()->void { ${growing_ring_setup} { var removed=ring.pop(); } left=3; ring.clear(); }" "borrow")
reject(ring_growth_result "${growing_ring} func main()->void { ${growing_ring_setup} var removed=ring.pop(); ring.clear(); right=3; var read=*removed.source; }" "borrow")
string(REPLACE "cursor+=1" "cursor+=2" growing_ring_skip "${growing_ring}")
reject(ring_growth_skipped "${growing_ring_skip} func main()->void { ${growing_ring_setup} ring.clear(); right=3; }" "borrow")
string(REPLACE "(head+cursor)%slots" "(head+cursor+1)%slots" growing_ring_shift "${growing_ring}")
reject(ring_growth_shifted "${growing_ring_shift} func main()->void { ${growing_ring_setup} ring.clear(); left=3; }" "borrow")
string(REPLACE "(head+cursor)%slots" "(head+cursor)%size" growing_ring_mod "${growing_ring}")
reject(ring_growth_modulus "${growing_ring_mod} func main()->void { ${growing_ring_setup} ring.clear(); left=3; }" "borrow")
string(REPLACE "head=0;" "head=1;" growing_ring_head "${growing_ring}")
reject(ring_growth_head "${growing_ring_head} func main()->void { ${growing_ring_setup} ring.clear(); left=3; }" "borrow")
reject(deque_growth_slot "struct Reader<'a> { var source:&'a i32; } func main()->void { var source:i32=1; match(collections.deque<Reader>(3)) { Ok(ring)=>{ ring.pushBack(Reader{source:&source}); var slot=ring.getRef(0); ring.ensureCapacity(8); var read=*(*slot).source; } Err(error)=>{} } }" "borrow")

set(rehash_table [=[
struct Reader<'a> { var source:&'a i32; }
struct Entry<'a> { var tag:u8; var key:Reader<'a>; var value:Reader<'a>; }
struct Table<'a> {
 var storage:*Entry<'a>; var limit:usize;
 func put(key:Reader<'a>,value:Reader<'a>)->void {
  var index:usize=(*key.source).(usize)%limit;
  while(storage[index].tag==7) { index=(index+1)%limit; }
  core.initialize(&storage[index],Entry{tag:7,key:key,value:value});
 }
 func grow(size:usize)->void {
  var fresh=core.core_alloc(size*sizeof(Entry)).(*Entry<'a>);
  if(fresh==core.null<Entry<'a>>()) { return; }
  for(var cursor:usize=0;cursor<size;cursor+=1) { fresh[cursor].tag=0; }
  for(var cursor:usize=0;cursor<limit;cursor+=1) {
   if(storage[cursor].tag==7) {
    var index:usize=(*storage[cursor].key.source).(usize)%size;
    while(fresh[index].tag==7) { index=(index+1)%size; }
    core.initialize(&fresh[index],take(storage[cursor]));
   }
  }
  core.core_release(storage.(*u8)); storage=fresh; limit=size;
 }
 func clear()->void { for(var index:usize=0;index<limit;index+=1) { if(storage[index].tag==7) { core.destroy(&storage[index]); } storage[index].tag=0; } }
 destructor { core.core_release(storage.(*u8)); }
}
]=])
set(rehash_setup "var key:i32=1; var source:i32=2; var table=Table{storage:core.core_alloc(3*sizeof(Entry)).(*Entry),limit:3}; for(var index:usize=0;index<3;index+=1) { table.storage[index].tag=0; } table.put(Reader{source:&key},Reader{source:&source}); table.grow(4);")
reject(rehash_live_value "${rehash_table} func main()->void { ${rehash_setup} source=3; var read=*table.storage[1].value.source; table.clear(); }" "borrow")
reject(rehash_live_key "${rehash_table} func main()->void { ${rehash_setup} key=3; var read=*table.storage[1].key.source; table.clear(); }" "borrow")
reject(rehash_copied_value "${rehash_table} func main()->void { ${rehash_setup} var copy=table.storage[1].value; table.clear(); source=3; var read=*copy.source; }" "borrow")
string(REPLACE "cursor<limit" "cursor<limit-1" rehash_short "${rehash_table}")
reject(rehash_short_scan "${rehash_short} func main()->void { ${rehash_setup} table.clear(); source=3; }" "borrow")
string(REPLACE "if(storage[cursor].tag==7)" "if(storage[cursor].tag==6)" rehash_tag "${rehash_table}")
reject(rehash_wrong_tag "${rehash_tag} func main()->void { ${rehash_setup} table.clear(); source=3; }" "borrow")
string(REPLACE "take(storage[cursor])" "take(storage[cursor+1])" rehash_shift "${rehash_table}")
reject(rehash_shifted "${rehash_shift} func main()->void { ${rehash_setup} table.clear(); source=3; }" "borrow")
string(REPLACE "index=(index+1)%size" "index=index+1" rehash_unbounded "${rehash_table}")
reject(rehash_unbounded_probe "${rehash_unbounded} func main()->void { ${rehash_setup} table.clear(); source=3; }" "borrow")

set(finisher [=[interface Finisher { once func finish()->i32; func peek()->i32; } struct FinishOwner { var resource:Resource; func finish()->i32 { return resource.value; } func peek()->i32 { return resource.value; } }]=])
reject(interface_once_second "${finisher} func main()->void { var value:Finisher=FinishOwner{resource:Resource{value:1}}; value.finish(); value.finish(); }" "moved|consum")
reject(interface_once_shared_borrow "${finisher} func main()->void { var value:Finisher=FinishOwner{resource:Resource{value:1}}; var reference=&value; (*reference).finish(); }" "owned|reference|borrow")
reject(interface_once_mut_borrow "${finisher} func main()->void { var value:Finisher=FinishOwner{resource:Resource{value:1}}; var reference=&mut value; (*reference).finish(); }" "owned|reference|borrow")
reject(interface_once_generic_second "${finisher} func invoke<F:Finisher>(value:F)->void { value.finish(); value.finish(); } func main()->void { invoke(FinishOwner{resource:Resource{value:1}}); }" "moved|consum")
reject(interface_once_generic_alias "${finisher} func invoke<F:Finisher>(value:F)->void { var moved=value; moved.finish(); moved.peek(); } func main()->void { invoke(FinishOwner{resource:Resource{value:1}}); }" "moved|consum")
reject(interface_once_generic_borrow "${finisher} func invoke<F:Finisher>(value:&F)->void { (*value).finish(); } func main()->void { var value=FinishOwner{resource:Resource{value:1}}; invoke(&value); }" "owned|reference|borrow")
reject(interface_once_generic_copy "interface Finish { once func finish()->i32; } struct CopyFinish { var value:i32; func finish()->i32 { return value; } } func invoke<F:Finish>(value:F)->void { value.finish(); value.finish(); } func main()->void { invoke(CopyFinish{value:1}); }" "moved|consum")
reject(interface_mut_generic_shared "interface Counter { mut func advance()->i32; } struct Count { var value:i32; func advance()->i32 { return value; } } func invoke<C:Counter>(value:&C)->i32 { return (*value).advance(); } func main()->void { var value=Count{value:1}; invoke(&value); }" "borrow|mutable|shared")
reject(interface_once_required_erasure "interface Finish { once func finish()->i32; } @[must_consume] struct Required { var value:i32; func finish()->i32 { return value; } } func main()->void { var value:Finish=Required{value:1}; value.finish(); }" "consum|convert|type")
reject(void_selection_discard "import (\"stdlib/async\"); async func nothing()->void {} func main()->void { var selected=block_on(asynchronous.selectVoid(nothing(),nothing())); }" "consum|Future|drop")

# Deliberately conservative limits: arbitrary key equality/index relationships
# are not established merely by running opaque comparison callbacks.
reject(limit_hash_key_release "struct Reader<'a> { var source:&'a i32; } func hash(key:i32)->u64 { return key.(u64); } func same(a:i32,b:i32)->bit { return a==b; } func main()->void { var left:i32=1; var right:i32=2; var values=collections.hashMap<i32,Reader>(hash,same); values.insert(1,Reader{source:&left}); values.insert(2,Reader{source:&right}); { var removed=values.remove(1); } left=3; var read=values.get(2); values.clear(); }" "borrow")
