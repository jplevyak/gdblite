use super::*;
use crate::graph::Options;

fn graph() -> Graph {
    Graph::open(None, Options::default()).unwrap()
}

fn tokens(text: &str) -> Vec<Token> {
    lex(text).unwrap().into_iter().map(|t| t.token).collect()
}

fn rows(out: &Output) -> &[Vec<u64>] {
    match out {
        Output::Rows { rows, .. } => rows,
        other => panic!("{other:?}"),
    }
}

/// Runs one statement.
fn one(g: &mut Graph, text: &str) -> Output {
    let mut out = execute(g, text).unwrap();
    assert_eq!(out.len(), 1);
    out.pop().unwrap()
}

/// The data of the values in each row.
fn data(g: &Graph, rows: &[Vec<u64>]) -> Vec<Vec<String>> {
    rows.iter()
        .map(|r| {
            r.iter()
                .map(|&id| String::from_utf8(g.data(id).unwrap_or(b"-").to_vec()).unwrap())
                .collect()
        })
        .collect()
}

#[test]
fn lexing() {
    assert_eq!(
        tokens("SELECT ?x, ? where /a/b knows _:n . -- comment\n \"a\\\"b\" 12 3.5 0x1f"),
        [
            Token::Keyword("select"),
            Token::Var("x".into()),
            Token::Punct(","),
            Token::Var("".into()),
            Token::Keyword("where"),
            Token::Key("/a/b".into()),
            Token::Ident("knows".into()),
            Token::Label("n".into()),
            Token::Punct("."),
            Token::Text("a\"b".into()),
            Token::Number("12".into()),
            Token::Number("3.5".into()),
            Token::Number("0x1f".into()),
            Token::End,
        ]
    );
    assert_eq!(
        tokens("a/* x */<= != ++ / [ ] ;"),
        [
            Token::Ident("a".into()),
            Token::Punct("<="),
            Token::Punct("!="),
            Token::Punct("++"),
            Token::Punct("/"),
            Token::Punct("["),
            Token::Punct("]"),
            Token::Punct(";"),
            Token::End,
        ]
    );
    // Positions.
    let t = lex("select\n  ?x").unwrap();
    assert_eq!((t[1].line, t[1].column), (2, 3));
    assert!(matches!(
        lex("select \"open"),
        Err(Error::Syntax {
            line: 1,
            column: 8,
            ..
        })
    ));
    assert!(matches!(lex("/* open"), Err(Error::Syntax { .. })));
    assert!(matches!(
        lex("select $"),
        Err(Error::Syntax { column: 8, .. })
    ));
}

#[test]
fn parsing() {
    let v = |s: &str| Node::Var(s.into());
    let val = |s: &str| Node::Value(s.into());
    let s = parse("select ?x where ?x a person ; knows ?y, /bob . ?y age 3").unwrap();
    assert_eq!(
        s,
        [Statement::Select {
            distinct: false,
            vars: Some(vec!["x".into()]),
            by: vec![],
            pattern: Pattern::And(vec![
                Pattern::And(vec![
                    Pattern::Triple(v("x"), val("type"), val("person")),
                    Pattern::Triple(v("x"), val("knows"), v("y")),
                    Pattern::Triple(v("x"), val("knows"), Node::Key("/bob".into())),
                ]),
                Pattern::And(vec![Pattern::Triple(v("y"), val("age"), val("3"))]),
            ]),
            limit: None,
        }]
    );
    // Blank nodes, named and not.
    let s = parse("verify ?x knows [ :?y age 3 ] ; likes [ name \"z\" ]").unwrap();
    let Statement::Verify(Pattern::And(subjects)) = &s[0] else {
        panic!("{s:?}")
    };
    let Pattern::And(ps) = &subjects[0] else {
        panic!()
    };
    assert_eq!(ps[0], Pattern::Triple(v("y"), val("age"), val("3")));
    assert_eq!(ps[1], Pattern::Triple(v("x"), val("knows"), v("y")));
    assert!(matches!(&ps[3], Pattern::Triple(_, p, Node::Blank(_)) if *p == val("likes")));
    // Several statements.
    assert_eq!(
        parse("verify ?x p ?y verify ?y q ?z quit select")
            .unwrap()
            .len(),
        2
    );
}

#[test]
fn statements() {
    let mut g = graph();
    let out = one(
        &mut g,
        r#"insert /alice a person ; name "Alice" ; age 30 ; knows /bob, /carol .
                  /bob a person ; name "Bob" ; age 25 ; knows /carol .
                  /carol a person ; name "Carol" ; age 35 ; banned true"#,
    );
    assert_eq!(out, Output::Inserted(13));
    // Inserting again adds nothing.
    assert_eq!(one(&mut g, "insert /alice knows /bob"), Output::Inserted(0));

    let out = one(&mut g, "select ?n by ?n where ?p a person ; name ?n");
    assert_eq!(data(&g, rows(&out)), [["Alice"], ["Bob"], ["Carol"]]);
    let Output::Rows { vars, .. } = one(&mut g, "select * where ?p name ?n ; age ?a") else {
        panic!()
    };
    assert_eq!(vars, ["p", "n", "a"]);

    // Comparisons of data, with literals or variables, and of ids.
    let out = one(
        &mut g,
        "select ?n by ?n where ?p name ?n ; age ?a . ?a >= 30",
    );
    assert_eq!(data(&g, rows(&out)), [["Alice"], ["Carol"]]);
    let out = one(&mut g, "select ?n where ?p name ?n ; age ?a . 30 > ?a");
    assert_eq!(data(&g, rows(&out)), [["Bob"]]);
    let out = one(
        &mut g,
        "select ?n, ?m where ?p name ?n ; age ?a ; knows ?q . ?q name ?m ; age ?b . ?a < ?b",
    );
    let mut pairs = data(&g, rows(&out));
    pairs.sort();
    assert_eq!(pairs, [["Alice", "Carol"], ["Bob", "Carol"]]);
    let out = one(
        &mut g,
        "select ?n where /alice knows ?p . ?p name ?n . ?p != /bob",
    );
    assert_eq!(data(&g, rows(&out)), [["Carol"]]);

    // Or, not, negated subjects and predicates.
    let out = one(
        &mut g,
        "select distinct ?p by ?n where ?p name ?n and (?p knows /carol or /alice knows ?p)",
    );
    assert_eq!(rows(&out).len(), 3);
    let out = one(&mut g, "select ?n where ?p name ?n . not ?p banned true");
    assert_eq!(data(&g, rows(&out)).len(), 2);
    let out = one(&mut g, "select ?n where ?p name ?n ; not banned true");
    assert_eq!(data(&g, rows(&out)).len(), 2);
    let out = one(&mut g, "select ?n where ?p name ?n and not (?p knows ?)");
    assert_eq!(data(&g, rows(&out)), [["Carol"]]);
    let out = one(&mut g, "select ?n limit 2 where ?p name ?n");
    assert_eq!(rows(&out).len(), 2);

    // Unknown constants match nothing.
    assert!(rows(&one(&mut g, "select ?x where ?x knows /nobody")).is_empty());

    assert_eq!(
        one(&mut g, "verify /alice knows /carol"),
        Output::Verified(true)
    );
    assert_eq!(
        one(&mut g, "verify /carol knows ?x"),
        Output::Verified(false)
    );

    // The links matched.
    let Output::Links(links) = one(&mut g, "lookup /alice knows ?x . ?x age ?a") else {
        panic!()
    };
    assert_eq!(links.len(), 4);
    for l in &links {
        assert!(g.get(*l).unwrap().is_link());
    }

    // Insert and delete for each match.
    let out = one(
        &mut g,
        "insert ?p friend ?q . ?p met [ at ?q ] where ?p knows ?q . ?q knows ?",
    );
    assert_eq!(out, Output::Inserted(3));
    let out = one(&mut g, "select ?p where ?p met ?m . ?m at /carol");
    assert_eq!(rows(&out).len(), 0);
    let out = one(&mut g, "select ?p where ?p met ?m . ?m at /bob");
    assert_eq!(rows(&out).len(), 1);
    assert_eq!(
        one(
            &mut g,
            "delete ?p knows ?q where ?p knows ?q . ?q banned true"
        ),
        Output::Deleted(2)
    );
    assert_eq!(
        one(&mut g, "verify ?p knows /carol"),
        Output::Verified(false)
    );
    assert_eq!(one(&mut g, "delete /alice knows /bob"), Output::Deleted(1));
    assert_eq!(one(&mut g, "delete /alice knows /bob"), Output::Deleted(0));
}

#[test]
fn same_as_queries() {
    // A GraQL query and the same query built by hand give the same rows.
    let mut g = graph();
    execute(
        &mut g,
        "insert /a p /b . /b p /c . /c p /a . /a q /c . /b q /a",
    )
    .unwrap();
    let (q, vars) = parse("select ?x, ?z where ?x p ?y . ?y p ?z . not ?x q ?z").unwrap()[0]
        .query(&g)
        .unwrap();
    assert_eq!(vars, ["x", "z"]);
    let [a, b, c] = ["/a", "/b", "/c"].map(|k| g.find_by_key(k.as_bytes()).unwrap());
    let (p, qq) = (g.find_value(b"p").unwrap(), g.find_value(b"q").unwrap());
    let mut hand = Query::new();
    let (x, y, z) = (hand.var("x"), hand.var("y"), hand.var("z"));
    let mut not = Group::new();
    not.pattern(x, qq, z);
    hand.pattern(x, p, y)
        .pattern(y, p, z)
        .not(not)
        .select([x, z]);
    let mut got = q.run(&g).unwrap();
    let mut expect = hand.run(&g).unwrap();
    got.sort();
    expect.sort();
    assert_eq!(got, expect);
    assert_eq!(got, [[c, b]]);
    let _ = (a, b);
}

#[test]
fn unsupported() {
    let unsupported = |text: &str| match parse(text) {
        Err(Error::Unsupported { what, .. }) => what,
        other => panic!("{text}: {other:?}"),
    };
    assert_eq!(
        unsupported("select ?x where ?x knows++ ?y"),
        "transitive predicates"
    );
    assert_eq!(
        unsupported("select ?x where ?x knows ?y - ; p ?z"),
        "predicate modifiers"
    );
    assert_eq!(
        unsupported("select ?x where ?x knows ?y limit 3 ; p ?z"),
        "predicate modifiers"
    );
    assert_eq!(
        unsupported("select ?x where ?x knows ?y ?author"),
        "author and subgraph positions"
    );
    assert_eq!(unsupported("select ?x where let ?x in ?x p ?y"), "'let'");
    assert_eq!(
        unsupported("select ?x where ?x p ?y implies ?x q ?y"),
        "'implies'"
    );
    assert_eq!(unsupported("select ?x where f(?x)"), "a call");
    assert_eq!(unsupported("select ?x where ?x p list(?a)"), "lists");
    assert_eq!(unsupported("rule r(?x) ?x p ?y"), "'rule'");
    assert_eq!(unsupported("insert ?x p ?y into g"), "a named graph");
    assert_eq!(unsupported("select ?x as y where ?x p ?y"), "'as'");
    assert!(matches!(
        parse("select ?x where\n  ?x p"),
        Err(Error::Syntax {
            line: 2,
            column: 7,
            ..
        })
    ));
    assert!(matches!(parse("select ?x"), Err(Error::Syntax { .. })));

    // Errors when run.
    let mut g = graph();
    assert!(matches!(
        execute(&mut g, "select ?y where ?x p ?z"),
        Err(Error::Syntax { .. })
    ));
    assert!(matches!(
        execute(&mut g, "select ?x where ?x p ?y or ?x q ?z"),
        Err(Error::Graph(_))
    ));
    assert!(execute(&mut g, "insert ?x p ?y where ?x q ?z").is_err());
}
