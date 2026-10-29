(* Test-only stand-in for OCaml 5's Domain module, so pfl.ml also builds on OCaml 4.14:
     cp domain_stub_ocaml4.ml domain.ml && ocamlopt domain.ml pfl.ml -o pfl
   It runs par sequentially. Delete domain.ml when you use OCaml 5. *)
type 'a t = ('a, exn) result
let spawn f = try Ok (f ()) with e -> Error e
let join = function Ok v -> v | Error e -> raise e
