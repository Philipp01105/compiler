func main() -> void
{
    print("Testing loop accumulation bug");

    var result:int = 1;
    print("Initial result: " + result);

    for (var i:int = 1; i <= 5; i++) {
        print("  Loop iteration: " + i);
        print("    Before: result = " + result);

        var temp:int = result * i;
        print("    temp = result * i = " + temp);

        result = temp;
        print("    After assignment: result = " + result);
    }

    print("Final result: " + result);
    print("Expected: 120");
}