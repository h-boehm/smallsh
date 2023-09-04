## smallsh

A small Unix-like shell implemented in C. The program provides an interactive prompt, parses command-line input into semantic tokens, and supports parameter and tilde expansion. It also implements shell-specific parameters (\$\$, \$?, \$!) and the built-in commands 'cd' and 'exit'. It handles I/O redirection with '<' and '>' operators, supports background processes with the '&' operator, and has custom behavior for handling SIGINT and SIGTSTP signals.
