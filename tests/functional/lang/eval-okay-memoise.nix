let
  # A function that logs its calls, so the expected stderr shows which
  # calls were memoised.
  traced = x: builtins.trace "called with ${builtins.toJSON x}" x;
  m = builtins.memoise traced;

  # Exponential without memoisation.
  fib = n: if n < 2 then n else builtins.memoise fib (n - 1) + builtins.memoise fib (n - 2);

  bad = throw "bad";

  getA = builtins.memoise (x: builtins.trace "called with a = ${toString x.a}" x.a);

  fail = builtins.memoise (x: builtins.trace "failing" (throw "error ${toString x}"));

  applyToOne = builtins.memoise (g: builtins.trace "applying" (g 1));
  inc = x: x + 1;
  mk = n: x: x + n;
in
[
  (fib 80)

  # Equal arguments are memoised, but integers and floats are distinct.
  (m 1)
  (m 1)
  (m 1.0)

  # Structurally equal attribute sets and lists.
  (m {
    a = 1;
    b = [
      2
      3
    ];
  })
  (m {
    b = [
      2
      3
    ];
    a = 1;
  })
  (m [ "foo" ])
  (m [ "foo" ])
  (m [ "bar" ])

  # Strings with different contexts are distinct.
  (builtins.memoise builtins.getContext "x")
  (builtins.memoise builtins.getContext (
    builtins.appendContext "x" {
      "${builtins.storeDir}/zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz-x" = {
        path = true;
      };
    }
  ))

  # Unused parts of the argument that fail to evaluate don't cause an
  # error. If they're identical, the calls are memoised, otherwise
  # they're not.
  (getA {
    a = 1;
    b = bad;
  })
  (getA {
    a = 1;
    b = bad;
  })
  (getA {
    a = 2;
    b = throw "bad";
  })
  (getA {
    a = 2;
    b = throw "bad";
  })

  # However, memoise is strict in its argument.
  (builtins.tryEval (builtins.memoise (x: 1) (throw "bad")))

  # Lambdas are equal only if their expression and environment
  # pointers are equal. So a different lambda expression with the same
  # body is unequal, and so are the results of two calls to `mk`, since
  # each call creates a new environment.
  (applyToOne inc)
  (applyToOne inc)
  (applyToOne (x: x + 1))
  (applyToOne (mk 1))
  (applyToOne (mk 1))

  # Built-in functions, partial applications and functors.
  (builtins.memoise builtins.toString 3)
  (builtins.memoise (builtins.add 1) 2)
  (builtins.memoise {
    __functor = self: x: x + self.y;
    y = 10;
  } 5)

  # Errors are memoised as well.
  (builtins.tryEval (fail 1))
  (builtins.tryEval (fail 1))
]
