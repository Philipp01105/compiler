"""Read-only workspace protocol integration, run with a freshly built compiler."""
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

COMPILER = str(pathlib.Path(sys.argv.pop(1)).resolve())


class WorkspaceIdeTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="dmm workspace ")
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)
        self.manifest = self.root / "dmm.manifest"
        self.manifest.write_text("module example/workspace\ndmm 2026-10-04-dev\n", encoding="utf-8")
        self.source = self.root / "main.dmm"
        self.source.write_text("package main; func main() -> int { return 0; }", encoding="utf-8")

    def request(self, **fields):
        request = {"protocol": "dmm-ide-v1", "requestId": "test", "operation": "analyze", "root": self.source.as_uri(), **fields}
        path, reply = self.root / "request.json", self.root / "response.json"
        path.write_text(json.dumps(request), encoding="utf-8")
        result = subprocess.run([COMPILER, "--ide-request", str(path), "--ide-response", str(reply)], capture_output=True, timeout=15)
        self.assertEqual(result.stdout, b"")
        self.assertEqual(result.stderr, b"")
        return result.returncode, json.loads(reply.read_text(encoding="utf-8"))

    def test_capabilities_and_protocol_errors(self):
        capabilities = json.loads(subprocess.check_output([COMPILER, "--ide-capabilities"]))
        self.assertIn("workspace-analysis", capabilities["capabilities"])
        status, reply = self.request(protocol="future")
        self.assertEqual(status, 2)
        self.assertIn("error", reply)
        self.assertEqual(reply["requestId"], "test")

    def test_closure_capture_bindings_and_source_callers(self):
        for mode in ("move", "&", "&mut"):
            with self.subTest(mode=mode):
                text = f"package main;func main()->int{{var offset:int=3;var callback=func[{mode} offset](value:int)->int{{var result=offset+value;return result;}};return callback(2);}}"
                self.source.write_text(text, encoding="utf-8")
                status, reply = self.request()
                self.assertEqual(status, 0)
                self.assertEqual(reply["diagnostics"], [])
                declarations = [d for d in reply["declarations"] if d["uri"] == self.source.as_uri()]
                closures = [d for d in declarations if d.get("anonymous")]
                self.assertEqual(len(closures), 1)
                closure = closures[0]
                self.assertEqual(closure["nameRange"]["start"]["character"], text.index("func["))
                self.assertEqual(closure["parameters"], [{"name": "value", "type": "int"}])
                offset = next(d for d in declarations if d["name"] == "offset")
                self.assertEqual(len([d for d in declarations if d["name"] == "offset"]), 1)
                for name in ("value", "result"):
                    symbol = next(d for d in declarations if d["name"] == name)
                    self.assertEqual(symbol["owner"], closure["id"])
                    uses = [o for o in reply["occurrences"] if o["range"]["start"]["character"] == text.rindex(name)]
                    self.assertEqual({o["declaration"] for o in uses}, {symbol["id"]})
                    self.assertEqual({o["caller"] for o in uses}, {closure["id"]})
                uses = [o for o in reply["occurrences"] if o["range"]["start"]["character"] == text.index("offset+value")]
                self.assertEqual({o["declaration"] for o in uses}, {offset["id"]})
                self.assertEqual({o["caller"] for o in uses}, {closure["id"]})
                calls = [o for o in reply["occurrences"] if o["role"] == "call" and
                         o["range"]["start"]["character"] == text.index("callback(2)")]
                self.assertEqual({o["declaration"] for o in calls}, {closure["id"]})
                self.assertEqual(self.source.read_text(encoding="utf-8"), text)

    def test_generic_instances_share_exact_source_bindings(self):
        text = "package main;func identity<T>(value:T)->T{var result=value;return result;}func main()->int{var count=identity<int>(3);var ratio=identity<double>(2.5);return count;}"
        self.source.write_text(text, encoding="utf-8")
        status, reply = self.request()
        self.assertEqual(status, 0)
        self.assertEqual(reply["diagnostics"], [])
        declarations = [d for d in reply["declarations"] if d["uri"] == self.source.as_uri()]
        identity = [d for d in declarations if d["name"] == "identity"]
        self.assertEqual(len(identity), 1)
        function = identity[0]
        self.assertEqual(function["returnType"], "T")
        self.assertEqual(function["parameters"], [{"name": "value", "type": "T"}])
        self.assertTrue(function["sourceOnly"])
        calls = [o for o in reply["occurrences"] if o["uri"] == self.source.as_uri() and o["role"] == "call"]
        self.assertEqual(len(calls), 2)
        self.assertEqual({o["declaration"] for o in calls}, {function["id"]})
        for name in ("value", "result"):
            symbols = [d for d in declarations if d["name"] == name]
            self.assertEqual(len(symbols), 1)
            self.assertEqual(symbols[0]["owner"], function["id"])
            uses = [o for o in reply["occurrences"] if o["uri"] == self.source.as_uri() and o["range"]["start"]["character"] == text.rindex(name)]
            resolved = [o for o in uses if o["declaration"] is not None]
            self.assertEqual({o["declaration"] for o in resolved}, {symbols[0]["id"]})
            self.assertEqual({o["caller"] for o in resolved}, {function["id"]})
        self.assertEqual(next(d for d in declarations if d["name"] == "value")["type"], "T")
        self.assertEqual(next(d for d in declarations if d["name"] == "result")["type"], "unknown")
        self.assertEqual(self.source.read_text(encoding="utf-8"), text)

    def test_generic_member_resolutions_retain_checked_contexts(self):
        text = "package main;struct A{var item:int;func read()->int{return item;}}struct B{var item:int;func read()->int{return item;}}func use<T>(value:T)->int{return value.read();}func main()->int{return use<A>(A{item:1})+use<B>(B{item:2});}"
        self.source.write_text(text, encoding="utf-8")
        status, reply = self.request()
        self.assertEqual(status, 0)
        self.assertEqual(reply["diagnostics"], [])
        function = next(d for d in reply["declarations"] if d["name"] == "use")
        contexts = {c["id"]: c for c in reply["checkedContexts"] if c["sourceDeclaration"] == function["id"]}
        self.assertEqual({tuple(c["parameterTypes"]) for c in contexts.values()}, {("A",), ("B",)})
        uses = [o for o in reply["occurrences"] if o["range"]["start"]["character"] == text.index("value.read") + 6 and o["declaration"] is not None]
        self.assertEqual(len({o["declaration"] for o in uses}), 2)
        self.assertEqual({o["checkedContext"] for o in uses}, set(contexts))
        self.assertEqual({o["caller"] for o in uses}, {function["id"]})
        self.assertTrue(reply["graphTraversalComplete"])
        coverage = reply["bindingCoverage"]
        self.assertTrue(coverage["auditComplete"])
        self.assertFalse(coverage["complete"])
        unknown = next(u for u in coverage["uncovered"] if u["range"]["start"]["character"] == text.index("value.read") + 6)
        self.assertEqual(unknown["reason"], "Source binding is known only in checked contexts")

    def test_binding_audit_covers_ownership_closure_and_async_sources(self):
        self.manifest.write_text("module example/workspace\ndmm 2026-10-04-dev\nfeatures = [\"async\"]\n", encoding="utf-8")
        text = "package main;func main()->int{var offset:int=3;var callback=func[&mut offset](value:int)->int{offset+=value;return offset;};return callback(2);}async func work(value:int)->int{var result=value;return result;}"
        self.source.write_text(text, encoding="utf-8")
        status, reply = self.request()
        self.assertEqual(status, 0)
        self.assertEqual(reply["diagnostics"], [])
        coverage = reply["bindingCoverage"]
        self.assertTrue(coverage["auditComplete"])
        self.assertTrue(coverage["complete"], coverage["uncovered"])
        self.assertTrue(coverage["generatedBindings"])
        self.assertGreater(coverage["sourceUseCount"], 0)

    def test_unbound_template_call_is_not_complete_graph_coverage(self):
        text = "package main;func unused<T>(value:T)->int{return value.missing();}func main()->int{return 0;}"
        self.source.write_text(text, encoding="utf-8")
        status, reply = self.request()
        self.assertEqual(status, 0)
        self.assertEqual(reply["diagnostics"], [])
        self.assertTrue(reply["graphTraversalComplete"])
        coverage = reply["bindingCoverage"]
        self.assertTrue(coverage["auditComplete"])
        self.assertFalse(coverage["complete"])
        function = next(d for d in reply["declarations"] if d["name"] == "unused")
        unknown = next(u for u in coverage["uncovered"] if u["role"] == "call")
        self.assertEqual(unknown["caller"], function["id"])
        self.assertEqual(unknown["range"]["start"]["character"], text.index("missing"))

    def test_ownership_operations_borrows_and_implicit_receivers_keep_source_uses(self):
        text = "package main;struct Resource{var value:int;func read()->int{return value;}}func main()->int{var original=Resource{value:3};var moved=take(original);var prior=replace(moved,Resource{value:4});{var view=&moved;}{var view=&mut moved;}return moved.value+prior.value;}"
        self.source.write_text(text, encoding="utf-8")
        status, reply = self.request()
        self.assertEqual(status, 0)
        self.assertEqual(reply["diagnostics"], [])
        self.assertTrue(reply["bindingCoverage"]["complete"], reply["bindingCoverage"]["uncovered"])
        uses = [o for o in reply["occurrences"] if o["uri"] == self.source.as_uri()]
        take = next(o for o in uses if o["range"]["start"]["character"] == text.index("take(original)") + 5)
        replace = next(o for o in uses if o["range"]["start"]["character"] == text.index("replace(moved") + 8)
        self.assertEqual(take["ownershipOperation"], 3)
        self.assertEqual(replace["ownershipOperation"], 4)
        self.assertEqual(take["accessBorrowKind"], 2)
        self.assertEqual(replace["accessBorrowKind"], 2)
        borrows = [o for o in uses if o["range"]["start"]["character"] in
                   (text.index("&moved") + 1, text.index("&mut moved") + 5)]
        self.assertEqual({o["accessBorrowKind"] for o in borrows}, {1, 2})
        implicit = next(o for o in uses if o["range"]["start"]["character"] == text.index("return value") + 7)
        self.assertTrue(implicit["implicitReceiver"])
        explicit = next(o for o in uses if o["range"]["start"]["character"] == text.index("return moved.value") + 13)
        self.assertFalse(explicit.get("implicitReceiver", False))
        self.assertEqual({o["intrinsic"] for o in uses if o["role"] == "call"}, {True})

    def test_async_call_and_await_bindings_keep_source_provenance(self):
        self.manifest.write_text("module example/workspace\ndmm 2026-10-04-dev\nfeatures = [\"async\"]\n", encoding="utf-8")
        text = "package main;async func work(value:int)->int{return value;}async func use(value:int)->int{var pending=work(value);return pending.await();}func main()->int{return 0;}"
        self.source.write_text(text, encoding="utf-8")
        status, reply = self.request()
        self.assertEqual(status, 0)
        self.assertEqual(reply["diagnostics"], [])
        self.assertTrue(reply["bindingCoverage"]["complete"], reply["bindingCoverage"]["uncovered"])
        work = next(d for d in reply["declarations"] if d["name"] == "work")
        call = next(o for o in reply["occurrences"] if o["uri"] == self.source.as_uri() and
                    o["range"]["start"]["character"] == text.index("=work") + 1)
        self.assertEqual(call["declaration"], work["id"])
        self.assertTrue(call["asyncCall"])
        pending = next(o for o in reply["occurrences"] if o["uri"] == self.source.as_uri() and
                       o["range"]["start"]["character"] == text.index("return pending") + 7)
        self.assertTrue(pending["await"])

    def test_structural_implementations_and_receiver_contracts(self):
        text = "package main;interface Reader{func read()->int;}interface Writer{mut func update()->int;}struct Counter{var value:int;func read()->int{return value;}func update()->int{value+=1;return value;}}struct Wrong{var value:int;func read()->double{return 1.0;}}func main()->int{return 0;}"
        self.source.write_text(text, encoding="utf-8")
        status, reply = self.request()
        self.assertEqual(status, 0)
        self.assertEqual(reply["diagnostics"], [])
        declarations = {d["id"]: d for d in reply["declarations"]}
        self.assertTrue(reply["implementationCoverage"]["ordinary"])
        relations = reply["implementationRelations"]
        self.assertEqual({(declarations[r["interface"]]["name"], declarations[r["implementation"]]["name"]) for r in relations}, {("Reader", "Counter"), ("Writer", "Counter")})
        for relation in relations:
            self.assertEqual(len(relation["methods"]), 1)
            method = relation["methods"][0]
            required, actual = declarations[method["required"]], declarations[method["implementation"]]
            self.assertEqual(required["owner"], relation["interface"])
            self.assertEqual(actual["owner"], relation["implementation"])
            self.assertEqual(required["name"], actual["name"])
            self.assertEqual(actual["receiverMode"], 1 if actual["name"] == "update" else 0)

    def test_generic_interface_relations_preserve_argument_contexts(self):
        text = "package main;interface Reader<T>{func read(value:T)->int;}struct IntegerReader{func read(value:int)->int{return value;}}struct DoubleReader{func read(value:double)->int{return value.(int);}}struct DualReader{func read(value:int)->int{return value;}func read(value:double)->int{return value.(int);}}struct WrongReceiver{var state:int;func read(value:int)->int{state+=value;return state;}}func integer<R:Reader<int>>(reader:R)->int{return reader.read(3);}func decimal<R:Reader<double>>(reader:R)->int{return reader.read(2.5);}func main()->int{var a:IntegerReader;var b:DoubleReader;return integer(a)+decimal(b)+integer(a);}"
        self.source.write_text(text, encoding="utf-8")
        status, reply = self.request()
        self.assertEqual(status, 0, reply["diagnostics"])
        self.assertEqual(reply["diagnostics"], [])
        declarations = {d["id"]: d for d in reply["declarations"]}
        contexts = {c["id"]: c for c in reply["interfaceContexts"]}
        self.assertEqual(len(contexts), 2, "Repeated bound checks share an argument context")
        self.assertTrue(reply["implementationCoverage"]["checkedGenericInterfaces"])
        relations = [r for r in reply["implementationRelations"] if "interfaceContext" in r]
        self.assertEqual({(tuple(contexts[r["interfaceContext"]]["argumentTypes"]), declarations[r["implementation"]]["name"]) for r in relations}, {(("int",), "IntegerReader"), (("double",), "DoubleReader"), (("int",), "DualReader"), (("double",), "DualReader")})
        for relation in relations:
            self.assertEqual(declarations[relation["interface"]]["name"], "Reader")
            self.assertEqual(len(relation["methods"]), 1)
            member = relation["methods"][0]
            self.assertEqual(declarations[member["required"]]["owner"], relation["interface"])
            self.assertEqual(declarations[member["implementation"]]["owner"], relation["implementation"])
            self.assertEqual(declarations[member["implementation"]]["parameters"][0]["type"], contexts[relation["interfaceContext"]]["argumentTypes"][0])
        self.assertEqual(self.source.read_text(encoding="utf-8"), text)

    def test_imported_generic_interface_contexts_keep_source_units(self):
        library = self.root / "library" / "lib.dmm"
        library.parent.mkdir()
        library.write_text("package library;pub interface Reader<T>{func read(value:T)->int;}pub func invoke<R:Reader<int>>(reader:R)->int{return reader.read(3);}", encoding="utf-8")
        text = 'package main;import "example/workspace/library";pub struct LocalReader{pub func read(value:int)->int{return value;}}func main()->int{var reader:LocalReader;return library.invoke(reader);}'
        self.source.write_text(text, encoding="utf-8")
        status, reply = self.request()
        self.assertEqual(status, 0)
        self.assertEqual(reply["diagnostics"], [])
        declarations = {d["id"]: d for d in reply["declarations"]}
        contexts = {c["id"]: c for c in reply["interfaceContexts"]}
        relations = [r for r in reply["implementationRelations"] if "interfaceContext" in r]
        self.assertEqual(len(relations), 1)
        relation = relations[0]
        self.assertEqual(contexts[relation["interfaceContext"]]["argumentTypes"], ["int"])
        self.assertEqual(declarations[relation["interface"]]["uri"], library.as_uri())
        self.assertEqual(declarations[relation["implementation"]]["name"], "LocalReader")
        self.assertEqual(self.source.read_text(encoding="utf-8"), text)

    def test_generic_interface_argument_arity_is_checked(self):
        for bound in ("Marker", "Marker<int,double>"):
            with self.subTest(bound=bound):
                text = f"package main;interface Marker<T>{{func read()->int;}}struct Candidate{{func read()->int{{return 1;}}}}func invoke<R:{bound}>(reader:R)->int{{return reader.read();}}func main()->int{{var reader:Candidate;return invoke(reader);}}"
                self.source.write_text(text, encoding="utf-8")
                status, reply = self.request()
                self.assertEqual(status, 0)
                self.assertTrue(reply["diagnostics"], "Incorrect generic arity must not satisfy an interface")
                self.assertFalse([r for r in reply["implementationRelations"] if "interfaceContext" in r])

    def test_generic_interface_parameter_bounds_are_checked(self):
        for argument, valid in (("Marked", True), ("Unmarked", False)):
            with self.subTest(argument=argument):
                text = f"package main;interface Marker{{func mark()->int;}}interface Reader<T:Marker>{{func read(value:T)->int;}}struct Marked{{func mark()->int{{return 1;}}}}struct Unmarked{{var value:int;}}struct Candidate{{func read(value:{argument})->int{{return 1;}}}}func invoke<R:Reader<{argument}>>(reader:R)->int{{return 0;}}func main()->int{{var reader:Candidate;return invoke(reader);}}"
                self.source.write_text(text, encoding="utf-8")
                status, reply = self.request()
                self.assertEqual(status, 0)
                if valid:
                    self.assertEqual(reply["diagnostics"], [])
                    self.assertTrue([r for r in reply["implementationRelations"] if "interfaceContext" in r])
                else:
                    self.assertTrue(reply["diagnostics"], "Interface type arguments must satisfy their own bounds")
                    self.assertFalse([r for r in reply["implementationRelations"] if "interfaceContext" in r])

    def test_unused_generic_interfaces_infer_all_overload_arguments(self):
        text = "package main;interface Reader<T>{func read(value:T)->int;}interface Missing<T>{func missing(value:T)->int;}struct Dual{func read(value:int)->int{return value;}func read(value:double)->int{return value.(int);}}struct Wrong{func read(value:int)->double{return 1.0;}}func main()->int{return 0;}"
        self.source.write_text(text, encoding="utf-8")
        status, reply = self.request()
        self.assertEqual(status, 0)
        self.assertEqual(reply["diagnostics"], [])
        declarations = {d["id"]: d for d in reply["declarations"]}
        contexts = {c["id"]: c for c in reply["interfaceContexts"]}
        self.assertTrue(reply["implementationCoverage"]["genericInterfaces"])
        self.assertEqual(reply["implementationCoverage"]["uncheckedInterfaces"], [])
        relations = [r for r in reply["implementationRelations"] if "interfaceContext" in r]
        self.assertEqual({tuple(contexts[r["interfaceContext"]]["argumentTypes"]) for r in relations}, {("int",), ("double",)})
        self.assertEqual({declarations[r["implementation"]]["name"] for r in relations}, {"Dual"})
        self.assertEqual(len({r["methods"][0]["implementation"] for r in relations}), 2)
        self.assertEqual(self.source.read_text(encoding="utf-8"), text)

    def test_generic_interface_wrapper_substitution_and_inference(self):
        fixtures = (
            ("&T", "&int", "int"),
            ("*T", "*int", "int"),
            ("T[]", "int[]", "int"),
            ("T[2]", "int[][2]", "int[]"),
            ("func(T)->int", "func(int)->int", "int"),
        )
        for pattern, actual, inferred in fixtures:
            with self.subTest(pattern=pattern):
                text = f"package main;interface Consumer<T>{{func consume(value:{pattern})->int;}}struct Candidate{{func consume(value:{actual})->int{{return 0;}}}}func main()->int{{return 0;}}"
                self.source.write_text(text, encoding="utf-8")
                status, reply = self.request()
                self.assertEqual(status, 0)
                self.assertEqual(reply["diagnostics"], [])
                self.assertTrue(reply["implementationCoverage"]["genericInterfaces"])
                self.assertEqual(len(reply["implementationRelations"]), 1)
                context = next(c for c in reply["interfaceContexts"] if c["id"] == reply["implementationRelations"][0]["interfaceContext"])
                self.assertEqual(context["argumentTypes"], [inferred])

    def test_unused_interface_parameters_are_explicit_wildcards(self):
        text = "package main;interface Tag<T>{func tag()->int;}struct Candidate{func tag()->int{return 1;}}func main()->int{return 0;}"
        self.source.write_text(text, encoding="utf-8")
        status, reply = self.request()
        self.assertEqual(status, 0)
        self.assertEqual(reply["diagnostics"], [])
        self.assertTrue(reply["implementationCoverage"]["genericInterfaces"])
        self.assertEqual(len(reply["implementationRelations"]), 1)
        self.assertEqual(reply["interfaceContexts"][0]["wildcardParameters"], 1)
        self.assertEqual(reply["interfaceContexts"][0]["argumentTypes"], ["*"])

    def test_unused_bounded_interface_parameters_preserve_domain_constraints(self):
        text = "package main;interface Marker{func mark()->int;}interface Conditional<T:Marker>{func inspect()->int;}struct Candidate{func inspect()->int{return 0;}}func main()->int{return 0;}"
        self.source.write_text(text, encoding="utf-8")
        status, reply = self.request()
        self.assertEqual(status, 0)
        self.assertEqual(reply["diagnostics"], [])
        conditional = next(d["id"] for d in reply["declarations"] if d["name"] == "Conditional")
        self.assertTrue(reply["implementationCoverage"]["genericInterfaces"])
        self.assertNotIn(conditional, reply["implementationCoverage"]["uncheckedInterfaces"])
        relation = next(r for r in reply["implementationRelations"] if r["interface"] == conditional)
        context = next(c for c in reply["interfaceContexts"] if c["id"] == relation["interfaceContext"])
        self.assertEqual(context["wildcardParameters"], 1)
        self.assertEqual(context["argumentTypes"], ["*"])
        self.assertEqual(context["parameterBounds"], [["Marker"]])

    def test_unused_bound_members_resolve_without_specialization(self):
        text = "package main;interface Reader{func read()->int;}func unused<T:Reader>(value:T)->int{var alias=value;return alias.read();}func main()->int{return 0;}"
        self.source.write_text(text, encoding="utf-8")
        status, reply = self.request()
        self.assertEqual(status, 0)
        self.assertEqual(reply["diagnostics"], [])
        self.assertTrue(reply["bindingCoverage"]["complete"], reply["bindingCoverage"]["uncovered"])
        reader = next(d for d in reply["declarations"] if d["name"] == "Reader")
        read = next(d for d in reply["declarations"] if d["name"] == "read" and d["owner"] == reader["id"])
        use = next(o for o in reply["occurrences"] if o["range"]["start"]["character"] == text.index("alias.read") + 6)
        self.assertEqual(use["declaration"], read["id"])
        self.assertEqual(use["role"], "call")

    def test_unused_nominal_generic_members_keep_declared_source_owner(self):
        text = "package main;struct Box<T>{var item:T;func get()->T{return item;}}func unused<U>(value:Box<U>)->U{return value.get();}func main()->int{return 0;}"
        self.source.write_text(text, encoding="utf-8")
        status, reply = self.request()
        self.assertEqual(status, 0)
        self.assertEqual(reply["diagnostics"], [])
        self.assertTrue(reply["bindingCoverage"]["complete"], reply["bindingCoverage"]["uncovered"])
        box = next(d for d in reply["declarations"] if d["name"] == "Box")
        get = next(d for d in reply["declarations"] if d["name"] == "get" and d["owner"] == box["id"])
        use = next(o for o in reply["occurrences"] if o["range"]["start"]["character"] == text.index("value.get") + 6)
        self.assertEqual(use["declaration"], get["id"])

    def test_multiple_bound_member_candidates_preserve_coverage_conflict(self):
        text = "package main;interface A{func read()->int;}interface B{func read()->int;}func unused<T:A+B>(value:T)->int{return value.read();}func main()->int{return 0;}"
        self.source.write_text(text, encoding="utf-8")
        status, reply = self.request()
        self.assertEqual(status, 0)
        self.assertEqual(reply["diagnostics"], [])
        self.assertFalse(reply["bindingCoverage"]["complete"])
        candidates = [o for o in reply["occurrences"] if o.get("sourceCandidate")]
        declarations = {d["id"]: d for d in reply["declarations"]}
        self.assertEqual({declarations[declarations[o["declaration"]]["owner"]]["name"] for o in candidates}, {"A", "B"})
        self.assertIn("contextual selection", reply["bindingCoverage"]["uncovered"][0]["reason"])

    def test_unused_generic_aggregate_implementation_contexts_keep_binders(self):
        text = "package main;interface Reader<T>{func read(value:T)->int;}struct Box<U>{func read(value:U)->int{return 0;}}struct Other<U>{func read(value:U)->int{return 1;}}struct Mutator<U>{var state:int;func read(value:U)->int{state+=1;return state;}}func main()->int{return 0;}"
        self.source.write_text(text, encoding="utf-8")
        status, reply = self.request()
        self.assertEqual(status, 0)
        self.assertEqual(reply["diagnostics"], [])
        self.assertTrue(reply["implementationCoverage"]["genericInterfaces"])
        declarations = {d["id"]: d for d in reply["declarations"]}
        contexts = {c["id"]: c for c in reply["interfaceContexts"]}
        relations = [r for r in reply["implementationRelations"] if "interfaceContext" in r]
        self.assertEqual({declarations[r["implementation"]]["name"] for r in relations}, {"Box", "Other"})
        self.assertEqual(len(relations), 2)
        for relation in relations:
            context = contexts[relation["interfaceContext"]]
            self.assertEqual(context["sourceOwner"], relation["implementation"])
            self.assertEqual(context["argumentTypes"], ["U"])
            self.assertEqual(declarations[relation["methods"][0]["implementation"]]["owner"], relation["implementation"])

    def test_unknown_template_receiver_effects_report_incomplete_coverage(self):
        text = "package main;interface Reader<T>{func read(value:T)->int;}func helper<T>(value:T)->int{return 0;}struct Box<U>{var state:U;func read(value:U)->int{return helper(state);}}func main()->int{return 0;}"
        self.source.write_text(text, encoding="utf-8")
        status, reply = self.request()
        self.assertEqual(status, 0)
        self.assertEqual(reply["diagnostics"], [])
        reader = next(d["id"] for d in reply["declarations"] if d["name"] == "Reader")
        self.assertFalse(reply["implementationCoverage"]["genericInterfaces"])
        self.assertIn(reader, reply["implementationCoverage"]["uncheckedInterfaces"])

    def test_source_generic_interface_bounds_use_declared_entailment(self):
        for bound, complete in ((":Marker", True), ("", False)):
            with self.subTest(bound=bound):
                text = f"package main;interface Marker{{func mark()->int;}}interface Reader<T:Marker>{{func read(value:T)->int;}}struct Box<U{bound}>{{func read(value:U)->int{{return 0;}}}}func main()->int{{return 0;}}"
                self.source.write_text(text, encoding="utf-8")
                status, reply = self.request()
                self.assertEqual(status, 0)
                self.assertEqual(reply["diagnostics"], [])
                reader = next(d["id"] for d in reply["declarations"] if d["name"] == "Reader")
                if complete:
                    self.assertTrue(reply["implementationCoverage"]["genericInterfaces"])
                    self.assertTrue([r for r in reply["implementationRelations"] if r["interface"] == reader])
                else:
                    self.assertIn(reader, reply["implementationCoverage"]["uncheckedInterfaces"])

    def test_source_receiver_contract_respects_shadowing_and_free_calls(self):
        text = "package main;interface Reader<T>{func read(value:T)->int;}func helper<T>(value:T)->int{return 0;}struct Box<U>{var state:int;func read(value:U)->int{var state:int=0;state+=1;return helper(value);}}func main()->int{return 0;}"
        self.source.write_text(text, encoding="utf-8")
        status, reply = self.request()
        self.assertEqual(status, 0)
        self.assertEqual(reply["diagnostics"], [])
        self.assertTrue(reply["implementationCoverage"]["genericInterfaces"])
        self.assertEqual(len(reply["implementationRelations"]), 1)
        actual = reply["implementationRelations"][0]["methods"][0]["implementation"]
        self.assertEqual(next(d for d in reply["declarations"] if d["id"] == actual)["receiverMode"], 0)

    def test_imported_source_generic_interface_bound_identities(self):
        library = self.root / "library" / "lib.dmm"
        library.parent.mkdir()
        library.write_text("package library;pub interface Marker{func mark()->int;}pub interface Reader<T:Marker>{func read(value:T)->int;}", encoding="utf-8")
        text = 'package main;import "example/workspace/library";struct Box<U:library.Marker>{func read(value:U)->int{return 0;}}func main()->int{return 0;}'
        self.source.write_text(text, encoding="utf-8")
        status, reply = self.request()
        self.assertEqual(status, 0)
        self.assertEqual(reply["diagnostics"], [])
        self.assertTrue(reply["implementationCoverage"]["genericInterfaces"])
        declarations = {d["id"]: d for d in reply["declarations"]}
        self.assertEqual(len(reply["implementationRelations"]), 1)
        relation = reply["implementationRelations"][0]
        self.assertEqual(declarations[relation["interface"]]["uri"], library.as_uri())
        self.assertEqual(declarations[relation["implementation"]]["name"], "Box")
        context = next(c for c in reply["interfaceContexts"] if c["id"] == relation["interfaceContext"])
        self.assertEqual(context["sourceOwner"], relation["implementation"])
        self.assertEqual(context["argumentTypes"], ["U"])

    def test_consuming_closure_captures_keep_original_binding(self):
        text = "package main;struct Resource{var value:int;}func main()->int{var owner=Resource{value:3};var callback=func[move owner]()->Resource{return owner;};var result=callback();return result.value;}"
        self.source.write_text(text, encoding="utf-8")
        status, reply = self.request()
        self.assertEqual(status, 0)
        self.assertEqual(reply["diagnostics"], [])
        declarations = reply["declarations"]
        closure = next(d for d in declarations if d.get("anonymous"))
        self.assertEqual(closure["parameters"], [])
        owner = next(d for d in declarations if d["name"] == "owner")
        self.assertEqual(len([d for d in declarations if d["name"] == "owner"]), 1)
        uses = [o for o in reply["occurrences"] if o["range"]["start"]["character"] == text.index("return owner") + 7]
        self.assertEqual({o["declaration"] for o in uses}, {owner["id"]})
        self.assertEqual({o["caller"] for o in uses}, {closure["id"]})
        calls = [o for o in reply["occurrences"] if o["role"] == "call" and
                 o["range"]["start"]["character"] == text.index("callback()")]
        self.assertEqual({o["declaration"] for o in calls}, {closure["id"]})

    def test_unused_generic_calls_use_compiler_package_visibility(self):
        library = self.root / "library" / "lib.dmm"
        library.parent.mkdir()
        library.write_text("package library;pub func read()->int{return 1;}func hidden()->int{return 2;}", encoding="utf-8")
        text = 'package main;import alias "example/workspace/library";const base:int=1;func helper()->int{return base;}func unused<T>(value:T)->int{return helper()+alias.read()+alias.hidden();}func main()->int{return 0;}'
        self.source.write_text(text, encoding="utf-8")
        status, reply = self.request()
        self.assertEqual(status, 0)
        self.assertEqual(reply["diagnostics"], [])
        declarations = {d["id"]: d for d in reply["declarations"]}
        for name, begin in (("helper", text.index("helper()+")), ("read", text.index("alias.read") + 6)):
            uses = [o for o in reply["occurrences"] if o["range"]["start"]["character"] == begin]
            self.assertEqual({declarations[o["declaration"]]["name"] for o in uses}, {name})
        hidden = [o for o in reply["occurrences"] if o["range"]["start"]["character"] == text.index("alias.hidden") + 6]
        self.assertTrue(hidden)
        self.assertEqual({o["declaration"] for o in hidden}, {None})
        self.assertFalse(reply["bindingCoverage"]["complete"])
        self.assertEqual([u["range"]["start"]["character"] for u in reply["bindingCoverage"]["uncovered"]],
                         [text.index("alias.hidden") + 6])

    def test_configured_stdlib_is_used_for_analysis_and_rename_validation(self):
        stdlib = self.root / "configured-stdlib"
        custom = stdlib / "custom" / "lib.dmm"
        custom.parent.mkdir(parents=True)
        (stdlib / "dmm.manifest").write_text("module stdlib\ndmm 2026-10-04-dev\n", encoding="utf-8")
        custom.write_text("package custom;pub func read()->int{return 7;}", encoding="utf-8")
        text = 'package library;import "stdlib/custom";func use()->int{var value=custom.read();return value;}'
        self.source.write_text(text, encoding="utf-8")
        status, reply = self.request(operation="workspace-analysis", stdlibRoot=str(stdlib))
        self.assertEqual(status, 0)
        self.assertEqual(reply["diagnostics"], [])
        self.assertIn(custom.as_uri(), {d["uri"] for d in reply["dependencies"]})
        status, reply = self.request(operation="rename", stdlibRoot=str(stdlib), uri=self.source.as_uri(),
                                     position={"line": 0, "character": text.rindex("value")}, newName="answer")
        self.assertEqual(status, 0)
        self.assertTrue(reply["rename"]["validated"], reply["rename"])
        self.assertEqual(self.source.read_text(encoding="utf-8"), text)
        self.assertEqual(custom.read_text(encoding="utf-8"), "package custom;pub func read()->int{return 7;}")
        status, reply = self.request(stdlibRoot=str(self.root / "missing-stdlib"))
        self.assertEqual(status, 0)
        self.assertTrue(any(d["uri"] == self.source.as_uri() and "Cannot load the module" in d["message"] for d in reply["diagnostics"]))
        status, reply = self.request(stdlibRoot=42)
        self.assertEqual(status, 2)
        self.assertIn("stdlibRoot", reply["error"]["message"])

    def test_workspace_directory_limit_is_explicit_failure(self):
        for index in range(4097):
            (self.root / f"empty-{index:04}").mkdir()
        status, reply = self.request(operation="workspace-analysis")
        self.assertEqual(status, 2)
        self.assertIn("discovery", reply["error"]["message"])
        self.assertNotIn("declarations", reply)

    def test_workspace_directory_inventory_tracks_empty_packages_and_module_boundaries(self):
        empty, nested = self.root / "empty", self.root / "nested"
        empty.mkdir()
        nested.mkdir()
        (nested / "dmm.manifest").write_text("module example/nested\ndmm 2026-10-04-dev\n", encoding="utf-8")
        (nested / "invalid.dmm").write_text("package nested;func broken()->int{return missing;}", encoding="utf-8")

        def fingerprint(directory, boundary):
            mask, prime = (1 << 64) - 1, 1099511628211
            total, count = 14695981039346656037, 0
            for entry in directory.iterdir():
                name = entry.name
                if name.startswith(".") or name == "vendor" or boundary and name != "dmm.manifest":
                    continue
                kind = b"d" if entry.is_dir() else b"f" if entry.is_file() else b"o"
                if kind != b"d" and name != "dmm.manifest" and not name.endswith(".dmm"):
                    continue
                value = 14695981039346656037
                for byte in name.encode("utf-8") + b"\0" + kind:
                    value = ((value ^ byte) * prime) & mask
                total = (total + value) & mask
                count += 1
            return f"fnv1a64-inventory:{((total ^ count) * prime) & mask:016x}"

        status, reply = self.request(operation="workspace-analysis")
        self.assertEqual(status, 0)
        inventories = {d["uri"]: d for d in reply["directoryDependencies"]}
        self.assertEqual(set(inventories), {self.root.as_uri(), empty.as_uri(), nested.as_uri()})
        self.assertTrue(inventories[nested.as_uri()]["boundaryOnly"])
        for directory in (self.root, empty, nested):
            entry = inventories[directory.as_uri()]
            self.assertEqual(entry["hash"], fingerprint(directory, entry["boundaryOnly"]))
        (empty / "notes.txt").write_text("not a package input", encoding="utf-8")
        self.assertEqual(inventories[empty.as_uri()]["hash"], fingerprint(empty, False))
        (empty / "new.dmm").write_text("package empty;func use()->int{return 1;}", encoding="utf-8")
        status, changed = self.request(operation="workspace-analysis")
        self.assertEqual(status, 0)
        updated = {d["uri"]: d for d in changed["directoryDependencies"]}
        self.assertNotEqual(inventories[empty.as_uri()]["hash"], updated[empty.as_uri()]["hash"])
        self.assertIn((empty / "new.dmm").as_uri(), changed["workspaceRoots"])
        (nested / "dmm.manifest").unlink()
        self.assertNotEqual(inventories[nested.as_uri()]["hash"], fingerprint(nested, True))
        status, overlaid = self.request(operation="workspace-analysis", manifestOverlays=[{
            "uri": (nested / "dmm.manifest").as_uri(), "version": 3,
            "text": "module example/nested\ndmm 2026-10-04-dev\n"}])
        self.assertEqual(status, 0)
        self.assertEqual(overlaid["diagnostics"], [])
        boundary = next(d for d in overlaid["directoryDependencies"] if d["uri"] == nested.as_uri())
        self.assertTrue(boundary["boundaryOnly"])
        self.assertEqual(boundary["hash"], fingerprint(nested, True))
        status, merged = self.request(operation="workspace-analysis")
        self.assertEqual(status, 0)
        self.assertTrue(any(d["uri"] == (nested / "invalid.dmm").as_uri() for d in merged["diagnostics"]))

    def test_workspace_discovery_selects_requested_target_variants(self):
        self.source.write_text("package library;func use()->int{return platform();}", encoding="utf-8")
        linux, windows = self.root / "platform_linux.dmm", self.root / "platform_windows.dmm"
        linux.write_text("package library;func platform()->int{return 1;}", encoding="utf-8")
        windows.write_text("package library;func platform()->int{return 2;}", encoding="utf-8")
        for target, selected, excluded in (("elf", linux, windows), ("coff", windows, linux)):
            status, reply = self.request(operation="workspace-analysis", target=target)
            self.assertEqual(status, 0)
            self.assertEqual(reply["diagnostics"], [])
            sources = {d["uri"] for d in reply["dependencies"]}
            self.assertIn(selected.as_uri(), sources)
            self.assertNotIn(excluded.as_uri(), sources)

    def test_workspace_discovery_includes_closed_reverse_callers(self):
        library = self.root / "library" / "lib.dmm"
        consumer = self.root / "consumer" / "use.dmm"
        library.parent.mkdir()
        consumer.parent.mkdir()
        library.write_text("package library;pub func read()->int{return 1;}", encoding="utf-8")
        saved = 'package consumer;import "example/workspace/library";func use()->int{return library.read();}'
        consumer.write_text(saved, encoding="utf-8")
        nested = self.root / "nested"
        nested.mkdir()
        (nested / "dmm.manifest").write_text("module example/nested\ndmm 2026-10-04-dev\n", encoding="utf-8")
        (nested / "wrong.dmm").write_text("package wrong;func invalid()->int{return missing;}", encoding="utf-8")
        status, reply = self.request(operation="workspace-analysis")
        self.assertEqual(status, 0)
        self.assertEqual(reply["diagnostics"], [])
        self.assertTrue(reply["workspaceDiscoveryComplete"])
        self.assertEqual(set(reply["workspaceRoots"]), {self.source.as_uri(), library.as_uri(), consumer.as_uri()})
        declarations = {d["id"]: d for d in reply["declarations"]}
        read = next(d for d in declarations.values() if d["name"] == "read")
        uses = [o for o in reply["occurrences"] if o["role"] == "call" and o["declaration"] == read["id"]]
        self.assertEqual({o["uri"] for o in uses}, {consumer.as_uri()})
        self.assertEqual({declarations[o["caller"]]["name"] for o in uses}, {"use"})
        changed = saved.replace("library.read()", "library.read()+library.read()")
        status, reply = self.request(operation="workspace-analysis", overlays=[{"uri": consumer.as_uri(), "text": changed, "version": 3}])
        self.assertEqual(status, 0)
        calls = [o for o in reply["occurrences"] if o["uri"] == consumer.as_uri() and o["role"] == "call"]
        self.assertEqual(len(calls), 2)
        self.assertEqual(consumer.read_text(encoding="utf-8"), saved)

    def test_mutable_capture_reads_and_writes_original_value(self):
        text = "package main;func main()->int{var offset:int=3;var callback=func[&mut offset](value:int)->int{offset+=value;return offset;};return callback(2);}"
        self.source.write_text(text, encoding="utf-8")
        status, reply = self.request()
        self.assertEqual(status, 0)
        self.assertEqual(reply["diagnostics"], [])
        declarations = [d for d in reply["declarations"] if d["uri"] == self.source.as_uri()]
        offset = next(d for d in declarations if d["name"] == "offset")
        closure = next(d for d in declarations if d.get("anonymous"))
        self.assertEqual(closure["receiverMode"], 1)
        self.assertEqual(next(d for d in declarations if d["name"] == "callback")["type"], "mut func(int) -> int")
        uses = [o for o in reply["occurrences"] if o["range"]["start"]["character"] == text.index("offset+=")]
        self.assertEqual({o["declaration"] for o in uses}, {offset["id"]})
        self.assertEqual({o["role"] for o in uses}, {"read", "write"})
        self.assertEqual({o["caller"] for o in uses}, {closure["id"]})

    def test_unused_generic_closure_and_async_source_scopes(self):
        self.manifest.write_text("module example/workspace\ndmm 2026-10-04-dev\nfeatures = [\"async\"]\n", encoding="utf-8")
        text = "package main;func unused<T>(value:T)->int{var callback=func[move value]()->T{return value;};return 0;}async func work(value:int)->int{var result=value;return result;}func main()->int{return 0;}"
        self.source.write_text(text, encoding="utf-8")
        status, reply = self.request()
        self.assertEqual(status, 0)
        self.assertEqual(reply["diagnostics"], [])
        declarations = reply["declarations"]
        closure = next(d for d in declarations if d.get("anonymous"))
        unused = next(d for d in declarations if d["name"] == "unused")
        value = next(d for d in declarations if d["name"] == "value" and d["owner"] == unused["id"])
        uses = [o for o in reply["occurrences"] if o["range"]["start"]["character"] == text.index("return value") + 7]
        self.assertEqual({o["declaration"] for o in uses}, {value["id"]})
        self.assertEqual({o["caller"] for o in uses}, {closure["id"]})
        work = next(d for d in declarations if d["name"] == "work")
        self.assertTrue(work["async"])
        result = next(d for d in declarations if d["name"] == "result")
        self.assertEqual(result["owner"], work["id"])
        uses = [o for o in reply["occurrences"] if o["range"]["start"]["character"] == text.index("return result") + 7]
        self.assertEqual({o["declaration"] for o in uses}, {result["id"]})
        self.assertEqual({o["caller"] for o in uses}, {work["id"]})

    def test_unused_templates_nested_binders_and_lifetimes(self):
        text = "package main;struct Box<T>{var item:T;func get()->T{return item;}}func unused<'a,T>(value:&'a T, callback:func<T>(T)->T)->&'a T{var result=value;{var result=value;}return result;}func main()->int{return 0;}"
        self.source.write_text(text, encoding="utf-8")
        status, reply = self.request()
        self.assertEqual(status, 0)
        self.assertEqual(reply["diagnostics"], [])
        self.assertTrue(reply["sourceRegistryComplete"])
        self.assertTrue(reply["bindingCoverage"]["templateDeclarations"])
        self.assertTrue(reply["bindingCoverage"]["auditComplete"])
        self.assertTrue(reply["bindingCoverage"]["complete"], reply["bindingCoverage"]["uncovered"])
        declarations = [d for d in reply["declarations"] if d["uri"] == self.source.as_uri()]
        function = next(d for d in declarations if d["name"] == "unused")
        self.assertTrue(function["sourceOnly"])
        self.assertEqual(function["genericParameters"], ["T"])
        self.assertEqual(function["lifetimeParameters"], ["'a"])
        self.assertEqual(function["returnType"], "&'a T")
        parameters = [d for d in declarations if d["kind"] == "type-parameter"]
        self.assertEqual(len(parameters), 3)
        outer = next(d for d in parameters if d["nameRange"]["start"]["character"] == text.index("'a,T") + 3)
        nested = next(d for d in parameters if d["nameRange"]["start"]["character"] == text.index("func<T>") + 5)
        lifetime = next(d for d in declarations if d["kind"] == "lifetime-parameter")
        self.assertNotEqual(outer["id"], nested["id"])
        self.assertNotEqual(outer["scope"], nested["scope"])
        uses = [o for o in reply["occurrences"] if o["uri"] == self.source.as_uri()]
        def binding_at(offset):
            return {o["declaration"] for o in uses if o["range"]["start"]["character"] == offset}
        self.assertEqual(binding_at(text.index("(T)->T") + 1), {nested["id"]})
        self.assertEqual(binding_at(text.index("(T)->T") + 5), {nested["id"]})
        self.assertEqual(binding_at(text.index("&'a T") + 4), {outer["id"]})
        self.assertEqual(binding_at(text.index("&'a T") + 1), {lifetime["id"]})
        self.assertEqual(binding_at(text.rindex("&'a T") + 1), {lifetime["id"]})
        result = next(d for d in declarations if d["name"] == "result" and d["nameRange"]["start"]["character"] == text.index("result=value"))
        self.assertEqual(binding_at(text.rindex("result")), {result["id"]})
        field = next(d for d in declarations if d["name"] == "item")
        self.assertEqual(binding_at(text.index("return item") + 7), {field["id"]})
        self.assertEqual(self.source.read_text(encoding="utf-8"), text)

    def test_template_local_initializer_uses_outer_binding(self):
        text = "package main;func unused<T>(value:T)->T{var result=value;{var result=result;}return result;}func main()->int{return 0;}"
        self.source.write_text(text, encoding="utf-8")
        status, reply = self.request()
        self.assertEqual(status, 0)
        self.assertEqual(reply["diagnostics"], [])
        declarations = [d for d in reply["declarations"] if d["uri"] == self.source.as_uri() and d["name"] == "result"]
        self.assertEqual(len(declarations), 2)
        outer = next(d for d in declarations if d["nameRange"]["start"]["character"] == text.index("result=value"))
        initializer = text.index("result=result") + len("result=")
        uses = [o for o in reply["occurrences"] if o["uri"] == self.source.as_uri() and o["range"]["start"]["character"] == initializer]
        self.assertEqual({o["declaration"] for o in uses}, {outer["id"]})

    def test_source_identity_is_independent_of_analysis_root(self):
        directory = self.root / "library"
        directory.mkdir()
        library = directory / "library.dmm"
        library.write_text("package library;pub func identity<T>(value:T)->T{return value;}", encoding="utf-8")
        self.source.write_text('package main;import "example/workspace/library";func main()->int{return library.identity<int>(3);}', encoding="utf-8")
        status, main = self.request()
        self.assertEqual(status, 0)
        status, standalone = self.request(root=library.as_uri())
        self.assertEqual(status, 0)
        selected = lambda graph: {d["name"]: d["identity"] for d in graph["declarations"] if d["uri"] == library.as_uri()}
        self.assertEqual(selected(main), selected(standalone))

    def test_root_overlay_and_exact_identities(self):
        original = self.source.read_bytes()
        text = "package main; func main()->int {var value=1; {var value=2; value=value+1;} return value;}"
        status, reply = self.request(overlays=[{"uri": self.source.as_uri(), "version": 4, "text": text}])
        self.assertEqual(status, 0)
        self.assertEqual(reply["diagnostics"], [])
        self.assertTrue(reply["ast"].startswith("dmm-ast-v6"))
        values = [d for d in reply["declarations"] if d["name"] == "value" and d["uri"] == self.source.as_uri()]
        self.assertEqual(len(values), 2)
        self.assertNotEqual(values[0]["identity"], values[1]["identity"])
        self.assertEqual({u["declaration"] for u in reply["occurrences"] if u["declaration"] in {d["id"] for d in values}}, {d["id"] for d in values})
        self.assertEqual(self.source.read_bytes(), original)
        self.assertIn(self.source.as_uri(), {d["uri"] for d in reply["dependencies"]})

    def test_generic_methods_link_to_methods_and_source_fields(self):
        text = "package main;struct Box<T>{var item:T;func get()->T{return item;}}func main()->int{var box:Box<int>;box.item=3;var other:Box<double>;other.item=2.5;var ratio=other.get();return box.get();}"
        self.source.write_text(text, encoding="utf-8")
        status, reply = self.request()
        self.assertEqual(status, 0)
        self.assertEqual(reply["diagnostics"], [])
        declarations = [d for d in reply["declarations"] if d["uri"] == self.source.as_uri()]
        boxes = [d for d in declarations if d["name"] == "Box"]
        methods = [d for d in declarations if d["name"] == "get"]
        fields = [d for d in declarations if d["name"] == "item"]
        self.assertEqual((len(boxes), len(methods), len(fields)), (1, 1, 1))
        self.assertEqual(methods[0]["kind"], "function")
        self.assertEqual(methods[0]["owner"], boxes[0]["id"])
        self.assertEqual(fields[0]["owner"], boxes[0]["id"])
        self.assertEqual(fields[0]["type"], "T")
        self.assertEqual(boxes[0]["nameRange"]["start"]["character"], text.index("Box"))
        occurrences = [o for o in reply["occurrences"] if o["uri"] == self.source.as_uri()]
        calls = [o for o in occurrences if o["role"] == "call"]
        self.assertEqual(len(calls), 2)
        self.assertEqual({o["declaration"] for o in calls}, {methods[0]["id"]})
        self.assertEqual({o["role"] for o in occurrences if o["range"]["start"]["character"] == text.index("box.item")}, {"read"})
        self.assertEqual({o["declaration"] for o in occurrences if o["range"]["start"]["character"] == text.index("return item") + 7 and o["declaration"] is not None}, {fields[0]["id"]})

    def test_function_signatures_and_inferred_types(self):
        text = "package main; func sum(left:int,right:i32)->int{return left;} func main()->int{var value=1;var explicit:int=2;return sum(value,explicit);}"
        status, reply = self.request(overlays=[{"uri": self.source.as_uri(), "version": 1, "text": text}])
        self.assertEqual(status, 0)
        declarations = {d["name"]: d for d in reply["declarations"] if d["uri"] == self.source.as_uri()}
        self.assertEqual(declarations["sum"]["parameters"], [{"name": "left", "type": "int"}, {"name": "right", "type": "i32"}])
        self.assertEqual(declarations["sum"]["returnType"], "int")
        self.assertFalse(declarations["sum"]["async"])
        self.assertTrue(declarations["value"]["inferred"])
        self.assertEqual(declarations["value"]["type"], "int")
        self.assertFalse(declarations["explicit"]["inferred"])
        calls = [o for o in reply["occurrences"] if o["role"] == "call" and o["declaration"] == declarations["sum"]["id"]]
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0]["caller"], declarations["main"]["id"])
        text = "package main; func read<'a>(value:&'a int)->int{return value;} func update(value:&mut int)->void{} func main()->int{var value=1;var reference=&value;return read(reference);}"
        status, reply = self.request(overlays=[{"uri": self.source.as_uri(), "version": 2, "text": text}])
        self.assertEqual(status, 0)
        declarations = {d["name"]: d for d in reply["declarations"] if d["uri"] == self.source.as_uri()}
        self.assertEqual(declarations["read"]["parameters"][0]["type"], "&'a int")
        self.assertEqual(declarations["update"]["parameters"][0]["type"], "&mut int")
        self.assertEqual(declarations["reference"]["type"], "&int")

    def test_unsaved_import_and_new_package_file(self):
        library = self.root / "library"
        library.mkdir()
        imported = library / "lib.dmm"
        imported.write_text("package library; pub func read()->int {return 1;}", encoding="utf-8")
        self.source.write_text('package main; import "example/workspace/library"; func main()->int {return library.read();}', encoding="utf-8")
        original = imported.read_bytes()
        status, reply = self.request(overlays=[{"uri": imported.as_uri(), "version": 1, "text": "package library; pub func read()->int {return missing;}"}])
        self.assertEqual(status, 0)
        self.assertTrue(any(d["uri"] == imported.as_uri() for d in reply["diagnostics"]))
        self.assertEqual(imported.read_bytes(), original)
        self.source.write_text('package main; import "example/workspace/library"; func main()->int {return library.extra();}', encoding="utf-8")
        new = library / "new.dmm"
        status, reply = self.request(overlays=[{"uri": new.as_uri(), "version": 1, "text": "package library; pub func extra()->int {return 2;}"}])
        self.assertEqual(status, 0)
        self.assertEqual(reply["diagnostics"], [])
        self.assertFalse(new.exists())
        extra = next(d for d in reply["declarations"] if d["uri"] == new.as_uri() and d["name"] == "extra")
        self.assertTrue(any(o["uri"] == self.source.as_uri() and o["declaration"] == extra["id"] for o in reply["occurrences"]))

    def test_unsaved_manifest_and_unicode(self):
        original = self.manifest.read_bytes()
        status, reply = self.request(manifestOverlays=[{"uri": self.manifest.as_uri(), "version": 2, "text": "module example/workspace\ndmm invalid\n"}])
        self.assertEqual(status, 0)
        self.assertTrue(reply["diagnostics"])
        self.assertEqual(self.manifest.read_bytes(), original)
        text = 'package main;\r\nfunc main()->int {var text="😀"; return missing;}\r\n'
        status, reply = self.request(overlays=[{"uri": self.source.as_uri(), "version": 3, "text": text}])
        diagnostic = next(d for d in reply["diagnostics"] if "missing" in d["message"])
        prefix = text.splitlines()[1].split("missing")[0]
        self.assertEqual(diagnostic["range"]["start"], {"line": 1, "character": len(prefix.encode("utf-16-le")) // 2})

    def test_protected_outputs_and_duplicate_overlay(self):
        request = self.root / "request.json"
        request.write_text(json.dumps({"protocol": "dmm-ide-v1", "requestId": "test", "operation": "analyze", "root": self.source.as_uri()}), encoding="utf-8")
        original = self.source.read_bytes()
        result = subprocess.run([COMPILER, "--ide-request", str(request), "--ide-response", str(self.source)], capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 2)
        self.assertEqual(self.source.read_bytes(), original)
        overlay = {"uri": self.source.as_uri(), "text": "package main;", "version": 0}
        status, reply = self.request(overlays=[overlay, overlay])
        self.assertEqual(status, 2)
        self.assertIn("Duplicate", reply["error"]["message"])

    def test_malformed_json_is_rejected(self):
        path, reply = self.root / "bad.json", self.root / "reply.json"
        for data in ['{', '{"text":"\\uD800"}', '{"text":"\\u0000"}', '{"a":0,"a":1}', '[1,]', '"\\']:
            path.write_text(data, encoding="utf-8")
            result = subprocess.run([COMPILER, "--ide-request", str(path), "--ide-response", str(reply)], capture_output=True, timeout=15)
            self.assertEqual(result.returncode, 2, data)
            self.assertIn("error", json.loads(reply.read_text(encoding="utf-8")))

    def test_malformed_request_cannot_overwrite_an_unrelated_source(self):
        request = self.root / "bad.json"
        request.write_text("{", encoding="utf-8")
        other = self.root / "unrelated.dmm"
        other.write_text("package unrelated;", encoding="utf-8")
        result = subprocess.run([COMPILER, "--ide-request", str(request), "--ide-response", str(other)], capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 2)
        self.assertEqual(other.read_text(encoding="utf-8"), "package unrelated;")

    def test_semantic_completion_uses_active_scope_and_receiver(self):
        text = "package main; func other(secret:int)->int{return secret;} func main(param:int)->int{var earlier=1; {var hidden=2;} return ear;}"
        cursor = text.index("return ear") + len("return ear")
        status, reply = self.request(operation="complete", uri=self.source.as_uri(), position={"line": 0, "character": cursor}, overlays=[{"uri": self.source.as_uri(), "version": 1, "text": text}])
        self.assertEqual(status, 0)
        names = {i["label"] for i in reply["items"]}
        self.assertTrue({"earlier", "param", "main", "other"}.issubset(names), names)
        self.assertTrue({"secret", "hidden"}.isdisjoint(names), names)
        member = "package main; struct Box {var value:int;func read()->int{return value;}} func main()->int{var box:Box;box.;return 0;}"
        cursor = member.index("box.;") + len("box.")
        status, reply = self.request(operation="complete", uri=self.source.as_uri(), position={"line": 0, "character": cursor}, overlays=[{"uri": self.source.as_uri(), "version": 2, "text": member}])
        self.assertEqual(status, 0)
        self.assertEqual({i["label"] for i in reply["items"]}, {"value", "read"})

    def test_type_references_use_semantic_declarations(self):
        text = "package main; struct Box {var value:int;} func use(box:Box)->Box{return box;} func main()->int{return 0;}"
        status, reply = self.request(overlays=[{"uri": self.source.as_uri(), "version": 1, "text": text}])
        box = next(d for d in reply["declarations"] if d["name"] == "Box" and d["uri"] == self.source.as_uri())
        uses = [o for o in reply["occurrences"] if o["uri"] == self.source.as_uri() and o["role"] == "type" and o["declaration"] == box["id"]]
        self.assertEqual(len(uses), 2)

    def rename(self, text, old, new):
        return self.request(operation="rename", uri=self.source.as_uri(), position={"line": 0, "character": text.rindex(old)}, newName=new,
                            overlays=[{"uri": self.source.as_uri(), "version": 1, "text": text}])

    def test_validated_local_and_parameter_rename(self):
        original = self.source.read_bytes()
        text = 'package main; func main()->int{var text="😀 value";var value=1;{var value=2;value=value+1;}return value;}'
        cursor = len(text[:text.rindex("value")].encode("utf-16-le")) // 2
        status, reply = self.request(operation="rename", uri=self.source.as_uri(), position={"line": 0, "character": cursor}, newName="answer",
                                    overlays=[{"uri": self.source.as_uri(), "version": 1, "text": text}])
        self.assertEqual(status, 0)
        self.assertTrue(reply["rename"]["validated"], reply["rename"])
        self.assertEqual(len(reply["rename"]["edits"]), 2)
        self.assertEqual(self.source.read_bytes(), original)
        parameter = "package main; func read(value:int)->int{return value;}func main()->int{return read(1);}"
        status, reply = self.rename(parameter, "value", "input")
        self.assertTrue(reply["rename"]["validated"], reply["rename"])
        self.assertEqual(len(reply["rename"]["edits"]), 2)

    def test_rename_rejects_binding_capture_even_if_proposed_code_compiles(self):
        text = "package main;func main()->int{var value=1;{var target=2;value=value+target;}return value;}"
        proposed = text.replace("value", "target")
        status, analysis = self.request(overlays=[{"uri": self.source.as_uri(), "version": 1, "text": proposed}])
        self.assertEqual(analysis["diagnostics"], [])
        status, reply = self.rename(text, "value", "target")
        self.assertFalse(reply["rename"]["validated"])
        self.assertNotIn("edits", reply["rename"])

    def test_rename_rejects_unsupported_symbols_and_conflicts(self):
        valid = "package main;func main()->int{var value=1;var other=2;return value+other;}"
        for name in ["other", "return", "not a name"]:
            status, reply = self.rename(valid, "value", name)
            self.assertFalse(reply["rename"]["validated"], name)
        status, reply = self.rename("package main;func main()->int{return 0;}", "main", "entry")
        self.assertFalse(reply["rename"]["validated"])

    def test_manifest_sync_preview_uses_dirty_manifest_without_writes(self):
        original = self.manifest.read_bytes()
        text = "// Keep module notes\n/* Module docs */\nmodule example/workspace\ndmm 2026-10-04-dev\nrequire example/unused v1.0.0 // indirect\n"
        status, reply = self.request(operation="sync-manifest", manifestOverlays=[{"uri": self.manifest.as_uri(), "version": 2, "text": text}])
        self.assertEqual(status, 0)
        plan = reply["manifestSync"]
        self.assertTrue(plan["validated"], reply["diagnostics"])
        self.assertEqual(plan["uri"], self.manifest.as_uri())
        self.assertEqual(plan["oldText"], text)
        self.assertIn("Module docs", plan["newText"])
        self.assertIn("Keep module notes", plan["newText"])
        self.assertNotIn("// indirect", plan["newText"])
        self.assertNotIn("example/unused", plan["newText"])
        self.assertEqual(self.manifest.read_bytes(), original)
        self.assertFalse(self.manifest.with_name("dmm.manifest.tmp").exists())
        self.assertIn(self.manifest.as_uri(), {entry["uri"] for entry in reply["dependencies"]})
        text = "// indirect\n" + text
        status, reply = self.request(operation="sync-manifest", manifestOverlays=[{"uri": self.manifest.as_uri(), "version": 3, "text": text}])
        self.assertTrue(reply["manifestSync"]["validated"])
        self.assertEqual(reply["manifestSync"]["newText"].count("// indirect"), 1)


if __name__ == "__main__":
    unittest.main()
