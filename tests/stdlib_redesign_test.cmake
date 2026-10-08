cmake_minimum_required(VERSION 3.21)
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
foreach(level 0 1)
    set(program "${OUTPUT_DIR}/redesign_${level}.exe")
    execute_process(COMMAND "${COMPILER}" "-O${level}" "${SOURCE_DIR}/tests/stdlib/redesign/redesign.dmm" -o "${program}"
        RESULT_VARIABLE status ERROR_VARIABLE errors TIMEOUT 90)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "Stdlib redesign O${level}: ${errors}")
    endif()
    execute_process(COMMAND "${program}" WORKING_DIRECTORY "${OUTPUT_DIR}" RESULT_VARIABLE status OUTPUT_VARIABLE output TIMEOUT 20)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "Stdlib redesign O${level} execution: ${status}, ${output}")
    endif()
    foreach(target elf coff)
        execute_process(COMMAND "${COMPILER}" "-O${level}" "--target=${target}" --emit=obj
            "${SOURCE_DIR}/tests/stdlib/redesign/redesign.dmm" -o "${OUTPUT_DIR}/redesign_${level}_${target}.o"
            RESULT_VARIABLE status ERROR_VARIABLE errors TIMEOUT 90)
        if(NOT status EQUAL 0)
            message(FATAL_ERROR "Stdlib redesign ${target} O${level}: ${errors}")
        endif()
    endforeach()
endforeach()
function(reject name source expected)
    set(directory "${OUTPUT_DIR}/${name}")
    file(MAKE_DIRECTORY "${directory}")
    file(WRITE "${directory}/dmm.manifest" "module redesign.reject/${name}\ndmm 2026-10-04-dev\n")
    file(WRITE "${directory}/main.dmm" "package main;\n${source}")
    execute_process(COMMAND "${COMPILER}" --emit=obj "${directory}/main.dmm" -o "${directory}/reject.o"
        RESULT_VARIABLE status ERROR_VARIABLE errors TIMEOUT 30)
    if(status EQUAL 0 OR NOT errors MATCHES "${expected}")
        message(FATAL_ERROR "Stdlib redesign ${name} rejection: ${status} ${errors}")
    endif()
endfunction()
foreach(component executor net threading system)
    reject(internal_${component}
        "import \"stdlib/internal/${component}\"; func main()->int{return 0;}"
        "internal")
endforeach()
reject(map_retained_reference [=[
import ("stdlib/core" "stdlib/collections");
struct Reader<'a>{var source:&'a int;}
func hash(value:&int)->u64{return (*value).(u64);}
func same(left:&int,right:&int)->bit{return *left==*right;}
func main()->int{
 var source:int=1;
 match(collections.map<int,Reader>(hash,same)){
  Ok(values)=>{
   match(values.insert(1,Reader{source:&source})){Ok=>{}Err(error)=>return 1;}
   source=2;var key:int=1;
   match(values.get(&key)){Some(value)=>return *value.source;None=>return 1;}
  }
  Err(error)=>return 1;
 }
}
]=] "borrow")
reject(result_default [=[
import "stdlib/core";
func main()->int {var value:core.Result<int,int>; match(value){Ok(number)=>return number;Err(error)=>return error;}}
]=] "initializ")
reject(mutable_item_escape [=[
import ("stdlib/core" "stdlib/collections");
func main()->int {
    var values=collections.list<int>();
    match(values.append(1)){Ok=>{} Err(e)=>return 1;}
    var destination:core.Option<&mut int>=core.Option<&mut int>.None;
    for(var &mut item=values) {destination=core.Option<&mut int>.Some(item);}
    match(destination){Some(item)=>return *item; None=>return 0;}
}
]=] "cannot escape its iteration")
reject(text_escape [=[
import ("stdlib/core" "stdlib/text");
func escape<'a>()->text.Text<'a> {
    match(text.fromString("owned")) {
        Ok(value)=>return value.view();
        Err(error)=>{
            var input:u8[1]=[1];var storage:u8[]=input[:];
            match(text.view(&storage)){Ok(view)=>return view;Err(e)=>return escape();}
        }
    }
}
func main()->int {return 0;}
]=] "cannot capture a borrow of a local|outlive|lifetimes")
reject(owner_value_iteration [=[
import ("stdlib/core" "stdlib/collections");
struct Owner {var value:int;destructor{}}
func main()->int {var values=collections.list<Owner>(); for(var item=values){} return 0;}
]=] "Copy|where|iter")
reject(fake_option [=[
import "stdlib/core";
enum Option<T>{None,Some(T)}
struct Cursor{func next()->Option<int>{return Option<int>.None;}}
struct Source{func iter()->Cursor{var result:Cursor;return result;}}
func main()->int{var source:Source;for(var value=source){}return 0;}
]=] "core.Option")
reject(map_lookup_escape [=[
import ("stdlib/core" "stdlib/collections");
func escape<'a>()->core.Option<&'a int> {
    match(collections.map<int,int>()) {
        Ok(values)=>{var key:int=1;return values.get(&key);}
        Err(error)=>return core.Option<&'a int>.None;
    }
}
func main()->int{match(escape()){Some(value)=>return *value;None=>return 0;}}
]=] "borrow|outlive|lifetimes")
reject(map_mutable_entry_alias [=[
import ("stdlib/core" "stdlib/collections");
func main()->int {
    match(collections.map<int,int>()){
        Ok(values)=>{
            match(values.insert(1,2)){Ok=>{}Err(e)=>return 1;}
            var cursor=values.entriesMut();
            match(cursor.next()){
                Some(first)=>match(cursor.next()){
                    Some(second)=>{*first.value=3;*second.value=4;return 0;}
                    None=>return 0;
                }
                None=>return 0;
            }
        }
        Err(e)=>return 1;
    }
}
]=] "borrow|loan")
reject(primitive_text_escape [=[
import ("stdlib/core" "stdlib/text");
func escape<'a>()->core.Result<text.Text<'a>,text.Error> {
    var number:int=1;var local="temporary"+number;
    var raw=text.bytes(local);return text.view(&raw);
}
func main()->int{match(escape()){Ok(view)=>return view.length().(int);Err(e)=>return 0;}}
]=] "borrow|outlive|lifetimes")
reject(map_entry_mutation [=[
import ("stdlib/core" "stdlib/collections");
func main()->int {
    match(collections.map<int,int>()) {
        Ok(values)=>{
            match(values.insert(1,2)){Ok=>{} Err(e)=>return 1;}
            var cursor=values.iter();
            match(cursor.next()){
                Some(entry)=>{values.clear();return *entry.value;}
                None=>return 0;
            }
        }
        Err(e)=>return 1;
    }
}
]=] "borrow|loan")
reject(map_captured_policy [=[
import ("stdlib/core" "stdlib/collections");
func equal(left:&int,right:&int)->bit{return *left==*right;}
func main()->int {
    var salt:u64=3;
    var policy=func [salt](value:&int)->u64{return (*value).(u64)+salt;};
    match(collections.map<int,int>(policy,equal)) {Ok(values)=>return 0;Err(e)=>return 1;}
}
]=] "incompatible|captur|assign|type")
reject(list_lookup_escape [=[
import ("stdlib/core" "stdlib/collections");
func escape<'a>()->&'a int {
    var values=collections.list<int>();
    match(values.append(1)){Ok=>{}Err(e)=>{}}
    return values.get(0);
}
func main()->int{return *escape();}
]=] "borrow|outlive|lifetimes")

reject(buffer_lookup_escape [=[
import ("stdlib/core" "stdlib/collections");
func escape<'a>()->core.Option<&'a int> {
    match(collections.buffer<int>(1,42)) {
        Ok(values)=>return core.Option<&'a int>.Some(values.get(0));
        Err(error)=>return core.Option<&'a int>.None;
    }
}
func main()->int{match(escape()){Some(value)=>return *value;None=>return 0;}}
]=] "borrow|outlive|lifetimes")
reject(list_view_escape [=[
import ("stdlib/core" "stdlib/collections");
func escape()->int[] {
    var values=collections.list<int>();
    match(values.append(1)){Ok=>{}Err(e)=>{}}
    return values.view();
}
func main()->int{var data=escape();return data[0];}
]=] "borrow|outlive|lifetimes|local")
reject(buffer_view_escape [=[
import ("stdlib/core" "stdlib/collections");
func escape()->int[] {
    match(collections.buffer<int>(1,42)) {
        Ok(values)=>return values.view();
        Err(error)=>return escape();
    }
}
func main()->int{var data=escape();return data[0];}
]=] "borrow|outlive|lifetimes|local")
reject(list_append_alias [=[
import ("stdlib/core" "stdlib/collections");
func main()->int {
    var values=collections.list<int>();
    match(values.append(1)){Ok=>{}Err(e)=>return 1;}
    var data=values.view();
    match(values.appendSlice(&data)){Ok=>return 0;Err(e)=>return 1;}
}
]=] "borrow|loan")

reject(byte_split_escape [=[
import ("stdlib/core" "stdlib/collections" byteops "stdlib/bytes");
func escape()->u8[]{
 var values=collections.list<u8>();match(values.append(65)){Ok=>{}Err(e)=>{}}
 var data=values.view();var cursor=byteops.split(&data,44);
 match(cursor.next()){Some(value)=>return value;None=>return escape();}
}
func main()->int{var data=escape();return data[0].(int);}
]=] "borrow|outlive|local")
reject(byte_split_mutation [=[
import ("stdlib/core" "stdlib/collections" byteops "stdlib/bytes");
func main()->int{
 var values=collections.list<u8>();match(values.append(65)){Ok=>{}Err(e)=>return 1;}
 var data=values.view();var cursor=byteops.split(&data,44);values.clear();
 match(cursor.next()){Some(value)=>return value.length.(int);None=>return 0;}
}
]=] "borrow|loan")
