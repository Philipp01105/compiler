cmake_minimum_required(VERSION 3.21)
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
file(WRITE "${OUTPUT_DIR}/dmm.manifest" "module shared.test/language\ndmm 2026-09-22-dev\nfeatures = [\"async\"]\n")
function(accept name source)
    file(WRITE "${OUTPUT_DIR}/main.dmm" "package main;\n${source}\n")
    foreach(level 0 1)
        execute_process(COMMAND "${COMPILER}" "-O${level}" --dump-ir "${OUTPUT_DIR}/${name}_${level}.ir"
            "${OUTPUT_DIR}/main.dmm" -o "${OUTPUT_DIR}/${name}_${level}.exe"
            RESULT_VARIABLE status ERROR_VARIABLE errors TIMEOUT 30)
        if(NOT status EQUAL 0)
            message(FATAL_ERROR "${name} O${level} compile: ${errors}")
        endif()
        execute_process(COMMAND "${OUTPUT_DIR}/${name}_${level}.exe" RESULT_VARIABLE status TIMEOUT 20)
        if(NOT status EQUAL 0)
            message(FATAL_ERROR "${name} O${level} returned ${status}")
        endif()
    endforeach()
endfunction()
function(reject name source pattern)
    file(WRITE "${OUTPUT_DIR}/main.dmm" "package main;\n${source}\n")
    execute_process(COMMAND "${COMPILER}" -S "${OUTPUT_DIR}/main.dmm" -o "${OUTPUT_DIR}/${name}.s"
        RESULT_VARIABLE status ERROR_VARIABLE errors TIMEOUT 30)
    if(status EQUAL 0 OR NOT errors MATCHES "${pattern}")
        message(FATAL_ERROR "${name} expected ${pattern}: ${errors}")
    endif()
endfunction()
accept(properties [=[
auto interface Safe {}
@[Safe] type int;
struct Box<T> { var value:T; }
@[Safe where T: Safe] struct RawBox<T> { var value:*T; }
func require<T:Safe>(value:T) -> int { return 0; }
func main() -> int { var a:int[2]; var b:Box<int>; var c:RawBox<int>; return require(a)+require(b)+require(c); }
]=])
reject(duplicate [=[auto interface Safe {} @[Safe] @[Safe] struct A {} func main()->int{return 0;}]=] "Duplicate auto interface")
reject(members [=[auto interface Safe { func f()->void; } func main()->int{return 0;}]=] "cannot have members")
reject(no_fallback [=[auto interface Safe {} @[Safe where int: Safe] struct A {} func f<T:Safe>(v:T)->void{} func main()->int{var a:A; f(a); return 0;}]=] "No overload|No generic")
reject(no_primitive_fact [=[auto interface Safe {} func f<T:Safe>(v:T)->void{} func main()->int{f(1); return 0;}]=] "No overload|No generic")
reject(external_duplicate [=[auto interface Safe {} @[Safe] type int; @[Safe] type int; func main()->int{return 0;}]=] "Duplicate auto interface")
reject(declaration_duplicate [=[auto interface Safe {} @[Safe] struct A{} @[Safe] type A; func main()->int{return 0;}]=] "Duplicate auto interface")
accept(external_specialization [=[auto interface Safe{} struct A<T>{var data:*T;} @[Safe] type A<int>; func f<T:Safe>(v:T)->int{return 0;} func main()->int{var a:A<int>; return f(a);}]=])
reject(specialization_duplicate [=[auto interface Safe{} @[Safe] struct A<T>{var data:*T;} @[Safe] type A<int>; func main()->int{return 0;}]=] "Duplicate auto interface")
reject(orphan [=[import "stdlib/core"; import "stdlib/memory"; @[core.Send] type int; func main()->int{return 0;}]=] "must own")
reject(condition_cycle [=[auto interface Safe {} @[Safe where A: Safe] struct A {} func f<T:Safe>(v:T)->void{} func main()->int{var a:A; f(a); return 0;}]=] "No generic")
reject(mutual_condition_cycle [=[auto interface Safe {} @[Safe where B:Safe] struct A{} @[Safe where A:Safe] struct B{} func f<T:Safe>(v:T)->void{} func main()->int{var a:A; f(a); return 0;}]=] "No generic")
accept(seeded_dependency [=[auto interface Safe{} @[Safe] struct A{} @[Safe where A:Safe] struct B{} func f<T:Safe>(v:T)->int{return 0;} func main()->int{var b:B; return f(b);}]=])
reject(enum_property [=[auto interface Safe{} @[Safe] type int; enum E{None,Some(*int)} func f<T:Safe>(v:T)->void{} func main()->int{var e:E; f(e); return 0;}]=] "No generic")
reject(auto_value [=[auto interface Safe {} func main()->int{var a:Safe; return 0;}]=] "Unknown|concrete")
reject(uninitialized [=[import "stdlib/core"; import "stdlib/memory"; func main()->int{var s:memory.Shared<int>; var c=s.clone(); return 0;}]=] "uninitialized")
reject(transitive [=[@[no_default] struct R { var n:int; } struct W<T>{var value:T;} func main()->int{var a:W<R>; return a.value.n;}]=] "uninitialized")
reject(array [=[@[no_default] struct R { var n:int; } func main()->int{var a:R[2]; return a[0].n;}]=] "uninitialized")
reject(first_variant [=[@[no_default] struct R { var n:int; } enum E { Some(R), None } func main()->int{var a:E; match(a){Some(r)=>return r.n; None=>return 0;}}]=] "uninitialized")
accept(option_default [=[import "stdlib/core"; @[no_default] struct R{var n:int;} func main()->int{var r:core.Option<R>; match(r){Some(v)=>return 1; None=>return 0;}}]=])
reject(partial_init [=[@[no_default] struct R {var n:int;} func main()->int{var r:R; r.n=1; return r.n;}]=] "uninitialized")
reject(package_init [=[@[no_default] struct R{var n:int;} var r:R; func main()->int{return r.n;}]=] "uninitialized")
accept(empty_variant [=[@[no_default] struct R { var n:int; } enum E { None, Some(R) } func main()->int{var a:E; match(a){None=>return 0; Some(r)=>return 1;}}]=])
accept(lifetimes [=[
import "stdlib/core"; import "stdlib/memory";
var drops:int;
@[no_default] struct R { var n:int; destructor { drops+=n; } }
func make(n:int)->R { return R{n:n}; }
func test(flag:bit)->void {
    var r:R;
    if(flag){r=make(1);}
    r=make(2);
    core.destroy<R>(&r);
    core.initialize<R>(&r,make(3));
}
func heap()->void {
    var ptr=core.alloc<R>();
    core.initialize<R>(ptr,make(4));
    core.destroy<R>(ptr);
    core.initialize<R>(ptr,make(5));
    core.destroy<R>(ptr);
    core.release(ptr);
}
func main()->int { test(false); test(true); heap(); if(drops==20){return 0;} return drops; }
]=])
reject(double_destroy [=[import "stdlib/core"; import "stdlib/memory"; struct R{destructor{}} func main()->int{var r:R; core.destroy<R>(&r); core.destroy<R>(&r); return 0;}]=] "destroyed")
reject(after_destroy [=[import "stdlib/core"; import "stdlib/memory"; func main()->int{var n=1; core.destroy<int>(&n); return n;}]=] "destroyed")
reject(initialize_live [=[import "stdlib/core"; import "stdlib/memory"; func main()->int{var n=1; core.initialize<int>(&n,2); return 0;}]=] "without a live value")
accept(shared [=[
import "stdlib/core"; import "stdlib/memory";
var drops:int;
struct R { var n:int; destructor { drops+=n; } }
func make()->memory.Shared<R> { match(memory.shared(R{n:7})){Ok(s)=>return s; Err(e)=>core.core_trap();} }
func test()->int { var a:memory.Shared<R>; a=make(); var b=a.clone(); var c=b.clone(); var view=c.get(); return view.n; }
func main()->int { if(test()!=7 || drops!=7){return 1;} return 0; }
]=])
reject(escape [=[import "stdlib/core"; import "stdlib/memory"; func leak()->&int { match(memory.shared(1)){Ok(s)=>return s.get(); Err(e)=>core.core_trap();} } func main()->int{return 0;}]=] "outlive")
reject(payload_escape [=[import "stdlib/core"; import "stdlib/memory"; import "stdlib"; func leak()->core.Result<memory.Shared<int[]>,core.AllocError> {var local:int[2]; var view:int[]=local; return memory.shared(view);} func main()->int{return 0;}]=] "local value|outlive")
reject(borrow_destroy [=[import "stdlib/core"; import "stdlib/memory"; struct R{var n:int; destructor{}} func main()->int{var r:R; var v:&R=&r; core.destroy<R>(&r); return v.n;}]=] "while it is borrowed")
reject(borrow_alias_destroy [=[import "stdlib/core"; import "stdlib/memory"; func main()->int{var n=1; var p:&int=&n; core.destroy<int>(p); return n;}]=] "while it is borrowed")
accept(async_shared [=[
import "stdlib/core"; import "stdlib/memory";
async func read(s:memory.Shared<int>)->int { return *s.get(); }
async func join(h:JoinHandle<int>)->int { match(h.await()){Ok(n)=>return n; Err(e)=>return -1;} }
func main()->int { match(memory.shared(21)){Ok(s)=>{var executor=Executor.create(2); var a=executor.spawn(read(s.clone())); var b=executor.spawn(read(s.clone())); var n=executor.block_on(join(a))+executor.block_on(join(b)); block_on(executor.shutdown(ShutdownMode.Drain)); return n-42;} Err(e)=>return 1;} }
]=])
accept(cancel_shared [=[
import "stdlib/core"; import "stdlib/memory";
var drops:int;
struct R{var n:int; destructor{drops+=1;}}
async func read(s:memory.Shared<R>)->int{return s.get().n;}
func test()->int{match(memory.shared(R{n:7})){Ok(s)=>{block_on(cancel(read(s.clone()))); return s.get().n-7;} Err(e)=>return 1;}}
func main()->int{var n=test(); if(n==0 && drops==1){return 0;} return 1;}
]=])
reject(not_send [=[import "stdlib/core"; import "stdlib/memory"; struct R{var ptr:*int;} async func read(s:memory.Shared<R>)->int{return 0;} func main()->int{match(memory.shared(R{ptr:core.null<int>()})){Ok(s)=>{var h=spawn(read(s)); block_on(cancel(h));} Err(e)=>{}} return 0;}]=] "Send Future")
reject(tracked_partial_destroy [=[import "stdlib/core"; import "stdlib/memory"; struct R{destructor{}} struct A{var r:R;} func main()->int{var a:A; core.destroy<R>(&a.r); return 0;}]=] "partial lifetimes")
accept(atomic_shared [=[
import "stdlib/core"; import "stdlib/memory";
async func work(s:memory.Shared<core.AtomicUsize>)->void { for(var i=0;i<1000;i+=1){var c=s.clone(); var v=c.get(); var n=v.load(); while(v.compareExchange(n,n+1.(usize))!=n){n=v.load();}} }
async func joined(h:JoinHandle<void>)->void {var r=h.await();}
func main()->int {match(memory.shared(core.atomicUsize(0.(usize)))){Ok(s)=>{var executor=Executor.create(4); var a=executor.spawn(work(s.clone())); var b=executor.spawn(work(s.clone())); var c=executor.spawn(work(s.clone())); var d=executor.spawn(work(s.clone())); executor.block_on(joined(a)); executor.block_on(joined(b)); executor.block_on(joined(c)); executor.block_on(joined(d)); block_on(executor.shutdown(ShutdownMode.Drain)); var v=s.get(); if(v.load()==4000.(usize)){return 0;} return 1;} Err(e)=>return 1;}}
]=])
foreach(level 0 1)
    file(READ "${OUTPUT_DIR}/lifetimes_${level}.ir" ir)
    if(NOT ir MATCHES "opcode=init" OR NOT ir MATCHES "opcode=destroy" OR NOT ir MATCHES "storage=uninitialized")
        message(FATAL_ERROR "Lifetime distinctions were lost in typed IR O${level}")
    endif()
endforeach()

# Compile the actual library source in an isolated package to exercise private
# invariants and a deterministic allocator failure without modifying the stdlib.
file(MAKE_DIRECTORY "${OUTPUT_DIR}/memory")
configure_file("${CMAKE_CURRENT_LIST_DIR}/../stdlib/memory/shared.dmm" "${OUTPUT_DIR}/memory/shared.dmm" COPYONLY)
file(WRITE "${OUTPUT_DIR}/memory/probe.dmm" [=[package memory;
import "stdlib/core";
pub func overflowProbe()->void {match(shared(1)){Ok(s)=>{(*s.storage).references.store(18446744073709551615.(usize));var c=s.clone();}Err(e)=>core.core_trap();}}
]=])
file(WRITE "${OUTPUT_DIR}/main.dmm" "package main; import \"shared.test/language/memory\"; func main()->int{memory.overflowProbe(); return 0;}\n")
foreach(level 0 1)
    execute_process(COMMAND "${COMPILER}" "-O${level}" "${OUTPUT_DIR}/main.dmm" -o "${OUTPUT_DIR}/overflow_${level}.exe"
        RESULT_VARIABLE status ERROR_VARIABLE errors TIMEOUT 30)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "Overflow probe compile: ${errors}")
    endif()
    execute_process(COMMAND "${OUTPUT_DIR}/overflow_${level}.exe" RESULT_VARIABLE status TIMEOUT 10)
    if(status EQUAL 0 OR status MATCHES "timeout")
        message(FATAL_ERROR "Reference count overflow did not trap")
    endif()
endforeach()
file(READ "${OUTPUT_DIR}/memory/shared.dmm" allocator)
string(REPLACE "core.core_alloc(sizeof(SharedStorage<T>))" "core.core_null()" allocator "${allocator}")
file(WRITE "${OUTPUT_DIR}/memory/shared.dmm" "${allocator}")
accept(allocation_failure [=[
import "shared.test/language/memory";
var drops:int;
struct R{destructor{drops+=1;}}
func test()->int {var r:R; match(memory.shared(r)){Ok(s)=>return 1; Err(e)=>return 0;}}
func main()->int {var n=test(); if(n==0 && drops==1){return 0;} return 1;}
]=])
