(* PFL interpreter in OCaml 5: the same language as lang/ in the C++ project.

   Build:  ocamlopt pfl.ml -o pfl        (needs OCaml 5.0 or newer for Domain)
   Run:    ./pfl ../examples/fib.pf 3      (3 = par cutoff depth)

   par(a, b) evaluates a on a new domain (an OS thread with its own minor
   heap) while b runs on the current one. Domains are expensive to create,
   so the cutoff should stay small (2^cutoff domains at most). A real version
   would use a task pool such as Domainslib instead. *)

exception Error of string

let fail fmt = Printf.ksprintf (fun s -> raise (Error s)) fmt

(* ---------- Tokens ---------- *)

type token =
  | INT of int | IDENT of string
  | LET | REC | IN | IF | THEN | ELSE | FUN | PAR
  | LPAREN | RPAREN | COMMA | ARROW | ASSIGN
  | OP of string   (* + - * / % < <= > >= == != *)
  | EOF

let keyword = function
  | "let" -> LET | "rec" -> REC | "in" -> IN | "if" -> IF | "then" -> THEN
  | "else" -> ELSE | "fun" -> FUN | "par" -> PAR | w -> IDENT w

let tokenize (s : string) : token array =
  let n = String.length s in
  let toks = ref [] in
  let i = ref 0 in
  let is_digit c = c >= '0' && c <= '9' in
  let is_alpha c = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c = '_' in
  while !i < n do
    let c = s.[!i] in
    if c = ' ' || c = '\t' || c = '\n' || c = '\r' then incr i
    else if c = '#' then (while !i < n && s.[!i] <> '\n' do incr i done)
    else if is_digit c then begin
      let start = !i in
      while !i < n && is_digit s.[!i] do incr i done;
      toks := INT (int_of_string (String.sub s start (!i - start))) :: !toks
    end else if is_alpha c then begin
      let start = !i in
      while !i < n && (is_alpha s.[!i] || is_digit s.[!i]) do incr i done;
      toks := keyword (String.sub s start (!i - start)) :: !toks
    end else begin
      let two = if !i + 1 < n then String.sub s !i 2 else "" in
      let tok, len =
        match two with
        | "->" -> ARROW, 2
        | "<=" | ">=" | "==" | "!=" -> OP two, 2
        | _ ->
          (match c with
           | '(' -> LPAREN, 1 | ')' -> RPAREN, 1 | ',' -> COMMA, 1 | '=' -> ASSIGN, 1
           | '+' | '-' | '*' | '/' | '%' | '<' | '>' -> OP (String.make 1 c), 1
           | _ -> fail "unexpected character '%c'" c)
      in
      toks := tok :: !toks;
      i := !i + len
    end
  done;
  Array.of_list (List.rev (EOF :: !toks))

(* ---------- Syntax tree ---------- *)

(* One constructor per kind of expression. The compiler checks that every
   match handles all of them, which the C++ switch only warns about. *)
type expr =
  | Int of int
  | Var of string
  | BinOp of string * expr * expr
  | Neg of expr
  | If of expr * expr * expr
  | Let of string * expr * expr
  | LetPair of string * string * expr * expr
  | LetRec of string * string * expr * expr   (* name, first param, body, rest *)
  | Fun of string * expr
  | App of expr * expr
  | Pair of expr * expr
  | Par of expr * expr

(* ---------- Parser (same precedence levels as parser.cpp) ---------- *)

let parse (toks : token array) : expr =
  let pos = ref 0 in
  let peek () = toks.(!pos) in
  let advance () = incr pos in
  let expect t what = if peek () = t then advance () else fail "expected %s" what in
  let ident () = match peek () with
    | IDENT x -> advance (); x
    | _ -> fail "expected a name" in
  let rec params () = match peek () with
    | IDENT x -> advance (); x :: params ()
    | _ -> [] in
  let wrap ps body = List.fold_right (fun p acc -> Fun (p, acc)) ps body in

  let rec expr () =
    match peek () with
    | LET -> advance (); let_expr ()
    | IF ->
      advance ();
      let c = expr () in expect THEN "'then'";
      let t = expr () in expect ELSE "'else'";
      If (c, t, expr ())
    | FUN ->
      advance ();
      let ps = params () in
      if ps = [] then fail "expected a parameter after 'fun'";
      expect ARROW "'->'";
      wrap ps (expr ())
    | _ -> compare ()

  and let_expr () =
    if peek () = LPAREN then begin
      advance ();
      let a = ident () in expect COMMA "','";
      let b = ident () in expect RPAREN "')'"; expect ASSIGN "'='";
      let v = expr () in expect IN "'in'";
      LetPair (a, b, v, expr ())
    end else begin
      let is_rec = (peek () = REC) in
      if is_rec then advance ();
      let name = ident () in
      let ps = params () in
      expect ASSIGN "'='";
      let v = expr () in
      expect IN "'in'";
      match is_rec, ps with
      | true, [] -> fail "'let rec' needs a parameter"
      | true, p :: rest -> LetRec (name, p, wrap rest v, expr ())
      | false, _ -> Let (name, wrap ps v, expr ())
    end

  and compare () =
    let lhs = sum () in
    match peek () with
    | OP ("<" | "<=" | ">" | ">=" | "==" | "!=" as op) -> advance (); BinOp (op, lhs, sum ())
    | _ -> lhs

  and sum () =
    let rec loop lhs = match peek () with
      | OP ("+" | "-" as op) -> advance (); loop (BinOp (op, lhs, term ()))
      | _ -> lhs in
    loop (term ())

  and term () =
    let rec loop lhs = match peek () with
      | OP ("*" | "/" | "%" as op) -> advance (); loop (BinOp (op, lhs, unary ()))
      | _ -> lhs in
    loop (unary ())

  and unary () = match peek () with
    | OP "-" -> advance (); Neg (unary ())
    | _ -> app ()

  and app () =
    let rec loop f = match peek () with
      | INT _ | IDENT _ | LPAREN | PAR -> loop (App (f, atom ()))
      | _ -> f in
    loop (atom ())

  and atom () =
    match peek () with
    | INT n -> advance (); Int n
    | IDENT x -> advance (); Var x
    | PAR ->
      advance (); expect LPAREN "'(' after par";
      let a = expr () in expect COMMA "','";
      let b = expr () in expect RPAREN "')'";
      Par (a, b)
    | LPAREN ->
      advance ();
      let e = expr () in
      if peek () = COMMA then begin
        advance ();
        let b = expr () in expect RPAREN "')'"; Pair (e, b)
      end else (expect RPAREN "')'"; e)
    | _ -> fail "expected an expression"
  in
  let e = expr () in
  expect EOF "end of input";
  e

(* ---------- Values and evaluation ---------- *)

(* An environment is a plain immutable list. Sharing it between domains is
   safe for free: nothing can change it. The C++ version needs shared_ptr
   with atomic reference counts to get the same guarantee. *)
type value =
  | VInt of int
  | VPair of value * value
  | VClosure of closure
  | VBuiltin of string
and closure = { param : string; body : expr; env : env; self : string option }
and env = (string * value) list

let rec to_string = function
  | VInt n -> string_of_int n
  | VPair (a, b) -> "(" ^ to_string a ^ ", " ^ to_string b ^ ")"
  | VClosure _ -> "<function>"
  | VBuiltin _ -> "<builtin>"

let as_int = function VInt n -> n | _ -> fail "expected an integer"

let arith op x y =
  match op with
  | "+" -> x + y | "-" -> x - y | "*" -> x * y
  | "/" -> if y = 0 then fail "division by zero" else x / y
  | "%" -> if y = 0 then fail "division by zero" else x mod y
  | "<" -> Bool.to_int (x < y) | "<=" -> Bool.to_int (x <= y)
  | ">" -> Bool.to_int (x > y) | ">=" -> Bool.to_int (x >= y)
  | "==" -> Bool.to_int (x = y) | "!=" -> Bool.to_int (x <> y)
  | _ -> fail "bad operator %s" op

let rec eval ~cutoff depth (env : env) (e : expr) : value =
  let ev = eval ~cutoff depth env in
  match e with
  | Int n -> VInt n
  | Var x -> (try List.assoc x env with Not_found -> fail "unknown name '%s'" x)
  | Neg a -> VInt (- (as_int (ev a)))
  | BinOp (op, a, b) -> let x = as_int (ev a) in VInt (arith op x (as_int (ev b)))
  | If (c, t, f) -> if as_int (ev c) <> 0 then ev t else ev f
  | Let (x, v, body) -> eval ~cutoff depth ((x, ev v) :: env) body
  | LetPair (a, b, v, body) ->
    (match ev v with
     | VPair (va, vb) -> eval ~cutoff depth ((b, vb) :: (a, va) :: env) body
     | _ -> fail "let (a, b) needs a pair")
  | LetRec (f, p, body, rest) ->
    let clo = VClosure { param = p; body; env; self = Some f } in
    eval ~cutoff depth ((f, clo) :: env) rest
  | Fun (p, body) -> VClosure { param = p; body; env; self = None }
  | App (f, a) -> apply ~cutoff depth (ev f) (ev a)
  | Pair (a, b) -> let va = ev a in VPair (va, ev b)
  | Par (a, b) ->
    if depth >= cutoff then
      let va = eval ~cutoff (depth + 1) env a in
      VPair (va, eval ~cutoff (depth + 1) env b)
    else begin
      let d = Domain.spawn (fun () -> eval ~cutoff (depth + 1) env a) in
      let vb = eval ~cutoff (depth + 1) env b in
      (* join re-raises an exception from the other domain *)
      VPair (Domain.join d, vb)
    end

and apply ~cutoff depth f arg =
  match f with
  | VBuiltin "fst" -> (match arg with VPair (a, _) -> a | _ -> fail "fst expects a pair")
  | VBuiltin _ -> (match arg with VPair (_, b) -> b | _ -> fail "snd expects a pair")
  | VClosure c ->
    let env = match c.self with Some name -> (name, f) :: c.env | None -> c.env in
    eval ~cutoff depth ((c.param, arg) :: env) c.body
  | _ -> fail "trying to call something that is not a function"

let run ?(cutoff = 3) source =
  let program = parse (tokenize source) in
  eval ~cutoff 0 [ ("fst", VBuiltin "fst"); ("snd", VBuiltin "snd") ] program

let () =
  if Array.length Sys.argv < 2 then (prerr_endline "usage: pfl file.pf [cutoff]"; exit 2);
  let source = In_channel.with_open_text Sys.argv.(1) In_channel.input_all in
  let cutoff = if Array.length Sys.argv > 2 then int_of_string Sys.argv.(2) else 3 in
  match run ~cutoff source with
  | v -> print_endline (to_string v)
  | exception Error msg -> prerr_endline ("error: " ^ msg); exit 1
