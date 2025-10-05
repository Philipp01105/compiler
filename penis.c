func fibRecursive(n:int) -> int
{
    if (n <= 1) {
        return n;
    }

    var a:int = fibRecursive(n - 1);
    var b:int = fibRecursive(n - 2);
    return a + b;
}

func fibIterative(n:int) -> int
{
    if (n <= 1) {
        return n;
    }

    var prev:int = 0;
    var curr:int = 1;

    for (var i:int = 2; i <= n; i++) {
        var next:int = prev + curr;
        prev = curr;
        curr = next;
    }

    return curr;
}

func printFib(n:int, value:int) -> void
{
    print("F(" + n + ") = " + value);
}

func main() -> void
{

    for (var i:int = 0; i < 20; i++) {
        var fib:int = fibIterative(i);
        printFib(i, fib);
    }

    print("");
    print("========================================");
    print("");

    for (var i:int = 0; i < 15; i++) {
        var fib:int = fibRecursive(i);
        printFib(i, fib);
    }
}