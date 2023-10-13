#define  _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// global vars
int child_status;
int exit_status;
int error = 0;
pid_t process_id;
pid_t process_arr[6] = {-1, -1, -1, -1};

// for $? var
int shell_var;

// for background processes
pid_t bg_id;
bool background = false;

// for redirection
char *infile = NULL;
char *outfile = NULL;

// string search and replace
char *search_replace(char *haystack, const char *needle, const char *sub)
{
  size_t haystack_len = strlen(haystack);
  size_t const needle_len = strlen(needle);
  size_t const sub_len = strlen(sub);
  char *str = haystack;

  for (; (str = strstr(str, needle)); ) {
    ptrdiff_t off  = str - haystack;
    if (sub_len > needle_len) {
      char *new_haystack = realloc(haystack, sizeof(char) * (haystack_len + sub_len - needle_len + 1));
      if (!new_haystack) {
        return NULL;
      }
      haystack = new_haystack;
      str = haystack + off;
    }
    memmove(str + sub_len, str + needle_len, haystack_len + 1 - off - needle_len);
    memcpy(str, sub, sub_len);
    haystack_len = haystack_len + sub_len - needle_len;
    str += sub_len;
  }

  str = haystack;
  if (sub_len < needle_len) {
    char *new_haystack = realloc(haystack, sizeof(char) * (haystack_len + 1));
    if (!new_haystack) {
      return NULL;
    }
    haystack = new_haystack;
  }

  return str;
}

void shell_exit(int status) 
{
  fprintf(stderr, "\nexit\n");
  // send SIGINT to child processes in the same process group
  for (int i; i < 4; i++) {
    if (process_arr[i] != -1) {
      kill(process_arr[i], 2);
    }
  }
  exit(status);
}

void shell_cd(char **args) 
{
  // error, more than 2 arguments
  if (args[2] != NULL) {
    fprintf(stderr, "Invalid number of arguments for cd\n");
  }
  // home dir
  else if (args[1] == NULL) {
    char *home = getenv("HOME");
    int res = chdir(home);
    if (res == -1) {
      fprintf(stderr, "Error when calling chdir\n");
      error = 1;
    }
  }
  // path dir
  else {
    int res = chdir(args[1]);
    if (res == -1) {
      fprintf(stderr, "Error when calling chdir\n");
      error = 1;
    }
  }
}

void parse_args(char **args) 
{
  // get number of args
  int args_idx = 0;
  for (int i = 0; i < 512; i++) {
    if (args[i] == NULL) {
      break;
    }
    args_idx++;
  }
  // parse # (comments)
  int comments = 0;
  bool comment_flag = false;
  for (int i = 0; i < args_idx; i++) {
    int res = strcmp(args[i], "#");
    if (res == 0) {
      comment_flag = true;
    }
    if (comment_flag) {
      args[i] = NULL;
      comments++;
    }
  }
  // update args index
  args_idx -= comments;
  // parse & (background process)
  if (args_idx > 0) {
    int res = strcmp(args[args_idx - 1], "&");
    if (res == 0) {
      background = true;
      args[args_idx - 1] = NULL;
    }
  }
  // parse < and > (redirection)
  if (args_idx > 2) {
    int res1 = strcmp(args[args_idx - 2], "<");
    int res2 = strcmp(args[args_idx - 2], ">");
    if (res1 == 0) {
      infile = args[args_idx - 1]; 
      args[args_idx - 2] = NULL;
    }
    if (res2 == 0) {
      outfile = args[args_idx - 1]; 
      args[args_idx - 2] = NULL;
    }
  }
  // parse < and > (two steps back)
  args_idx -= 2;
  if (args_idx > 2) {
    int res1 = strcmp(args[args_idx - 2], "<");
    int res2 = strcmp(args[args_idx - 2], ">");
    if (res1 == 0) {
      infile = args[args_idx - 1]; 
      args[args_idx - 2] = NULL;
    }
    if (res2 == 0) {
      outfile = args[args_idx - 1]; 
      args[args_idx - 2] = NULL;
    }
  }
}

char **expand_tokens(char **tokens) 
{  
  // get number of tokens
  int tokens_len = 0;
  for (int i = 0; i < 512; i++) {
    if (tokens[i] == NULL) {
      break;
    }
    tokens_len++;
  }
  int digits_len;

  // for each token
  for (int i = 0; i < tokens_len; i++) {

    int str_len = strlen(tokens[i]);

    // loop through each char
    for (int j = 0; j <= str_len - 2; j++) {

      // if token contains "~/", replace with $HOME
      if (tokens[i][j] == '~' && tokens[i][j+1] == '/') {
        char *copy = strdup(tokens[i]);
        // get HOME var
        char *home = getenv("HOME");
        int home_len = strlen(home);
        // string replace
        char *ret = search_replace(copy, "~", home);
        tokens[i] = ret;
      }

      // if token contains "$$", replace with process ID of smallsh    
      if (tokens[i][j] == '$' && tokens[i][j+1] == '$') {
        char *copy = strdup(tokens[i]);
        pid_t pid = getpid();
        pid_t pid_cpy = pid;
        digits_len = 0;
        while(pid_cpy != 0) {
          pid_cpy = pid_cpy / 10;
          digits_len++;
        }
        char pid_str[digits_len + 1];
        sprintf(pid_str, "%d", pid);
        // string replace
        char *ret = search_replace(copy, "$$", pid_str);
        tokens[i] = ret;
      }

      // if token contains "$?", replace with exit status of last foreground command (or "0")
      if (tokens[i][j] == '$' && tokens[i][j+1] == '?') {
        char *copy = strdup(tokens[i]);
        if (shell_var > 0) {
          int status_cpy = shell_var;
          digits_len = 0;
          while(status_cpy != 0) {
            status_cpy = status_cpy / 10;
            digits_len++;
          }
          char status_str[digits_len + 1];
          sprintf(status_str, "%d", shell_var);
          // string replace
          char *ret = search_replace(copy, "$?", status_str);
          tokens[i] = ret;
        }
        else {
          char *ret = search_replace(copy, "$?", "0");
          tokens[i] = ret;
        }
      }

      // if token contains "$!", replace with pid of last bg process (or "")
      if (tokens[i][j] == '$' && tokens[i][j+1] == '!') {
        char *copy = strdup(tokens[i]);
        if (bg_id > 0) {
          int id_cpy = bg_id;
          digits_len = 0;
          while(id_cpy != 0) {
            id_cpy = id_cpy / 10;
            digits_len++;
          }
          char id_str[digits_len + 1];
          sprintf(id_str, "%d", bg_id);
          // string replace
          char *ret = search_replace(copy, "$!", id_str);
          tokens[i] = ret;
        }
        else {
          char *ret = search_replace(copy, "$!", "");
          tokens[i] = ret;
        } 
      }
    }
  }
  return tokens;
}

char **split_words(char *line) 
{
  // get IFS env var for delimiters
  char *IFS = getenv("IFS");
  // if IFS is unset
  if (IFS == NULL) {
    IFS = " \t\n";
  }
  // create tokens arr of pointers to tokens (512 words)
  char **tokens = (char**) malloc(512 * sizeof(char*));
  if (tokens == NULL) {
    fprintf(stderr, "Error when calling malloc\n");
    error = 1;
  }
  char *token;
  token = strtok(line, IFS);
  int index = 0;
  // returns NULL when there are no more tokens
  while (token != NULL) {
    // move through all tokens
    tokens[index] = token;
    index++;
    token = strtok(NULL, IFS);
  }
  // end tokens arr with NULL
  tokens[index] = NULL;  
  return tokens;
}

char *user_input() 
{
  // initialize FILE object for stdin
  FILE *input;
  input = stdin;
  // code adapted from SMALLSH Canvas page
  char *line = NULL;
  size_t n = 0;
  // input - read stdin with getline
  line = (char *)malloc(n * sizeof(char));
  ssize_t line_length = getline(&line, &n, stdin);
  // if error or EOF occurred
  if (line_length == -1) {
    if (errno = EOF) {
      // EOF on stdin implies exit $?
      shell_exit(process_id);
    }
    // if reading is interruped by signal
    // return to step 1
    else if (errno = EINTR) {
      fprintf(stderr, "\n");
      error = 1;
      errno = 0;
      clearerr(stdin);
    }
    else {
      fprintf(stderr, "Error when calling getline\n");
      error = 1;
    }
  }
  return line;
}

void print_prompt() 
{
  // print PS1 var to stderr
  char *PS1 = getenv("PS1");
  // if there is no match
  if (PS1 == NULL) {
    // if env var not set, interpret as empty string
    PS1 = "";
  }
  fprintf(stderr, "%s", PS1);
}

int main(int argc, char *argv[]) 
{
  while(1) {

    // check for background processes
    start: process_id = waitpid(0, &child_status, WNOHANG | WUNTRACED);

    // if exited
    if (WIFEXITED(child_status) && process_id > 0) {
      int exit_status = WEXITSTATUS(child_status);
      fprintf(stderr, "Child process %jd done. Exit status %d.\n", (intmax_t) process_id, exit_status);
    }
    // if signaled
    if (WIFSIGNALED(child_status)  && process_id > 0)  {
      int signal_num = WTERMSIG(child_status);
      fprintf(stderr, "Child process %jd done. Signaled %d.\n", (intmax_t) process_id, signal_num);
    }
    // if stopped
    if (WIFSTOPPED(child_status) && process_id > 0) {
      kill(process_id, 18);
      fprintf(stderr, "Child process %jd stopped. Continuing.\n", (intmax_t) process_id);
    }

    // print prompt
    print_prompt();

    // get user input
    char *line = user_input();

    // if error occurred, go to start
    if (error == 1) {
      error = 0;
      goto start;
    }

    // splitting
    char **tokens = split_words(line);

    // expansion
    char **args = expand_tokens(tokens);

    // parsing
    parse_args(args);

    // if error occurred, go to start
    if (error == 1) {
      error = 0;
      goto start;
    }

    if (args[0] != NULL) {

      // check for built-in commands
      int res = strcmp(args[0], "cd");
      int res2 = strcmp(args[0], "exit");
      // built-in cd
      if (res == 0) {
        shell_cd(args);
      }
      // built-in exit
      else if (res2 == 0) {
        // raise error for two arguments
        if (args[2] != NULL) {
          fprintf(stderr, "Invalid number of arguments for exit\n");
        }
        // raise error for non-integer
        // check each char of arg with isdigit
        int arg_len = strlen(args[1]);
        for (int i = 0; i < arg_len; i++) {
          int res3 = isdigit(args[1][i]);
          if (res3 == 0) {
            fprintf(stderr, "exit argument is not an integer\n");
            break;
          }
        }
        // pass second argument
        if (args[1] != NULL) {
        shell_exit(atoi(args[1]));
        }
        // else pass last exit status
        else {
          shell_exit(exit_status);
        }
      }

      // execute process
      else {
        // fork a new process
        pid_t process_id = fork();
        if (process_id == -1) {
          fprintf(stderr, "Error when calling fork\n");
        }
        int file_descriptor;

        switch(process_id) {

          case -1:
            perror("Error when calling fork\n");
            exit(1);
            break;

          case 0:
            // in the child process
            if (infile != NULL) {
              // open/create file from infile string var
              int file_descriptor = open(infile, O_RDONLY, 0777);
              if (file_descriptor == -1) {
                fprintf(stderr, "Error when calling open on \"%s\"\n", outfile);
                exit(1);
              }
              int res = dup2(file_descriptor, 0);
              if (res == -1) {
                fprintf(stderr, "Error when calling dup2\n");
              }
            }
            if (outfile != NULL) {
              // open/create file from outfile string
              int file_descriptor = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0600);
              if (file_descriptor == -1) {
                fprintf(stderr, "Error when calling open on \"%s\"\n", outfile);
                exit(1);
              }
              int res = dup2(file_descriptor, 1);
              if (res == -1) {
                fprintf(stderr, "Error when calling dup2\n");
              }
            }
            // replace the current program with args
            execvp(args[0], args);
            // exec only returns if there is an error
            perror("Error when calling execvp\n");
            outfile = NULL;
            infile = NULL;
            exit(2);
            break;
        
          default:
            // foreground process
            if (!background) {
              process_id = waitpid(process_id, &child_status, 0);
              if (WIFEXITED(child_status)) {
                  exit_status = WEXITSTATUS(child_status);
                  shell_var = exit_status;
              }
              if WIFSIGNALED(child_status) {
                shell_var = WTERMSIG(child_status) + 128;
              }
              if WIFSTOPPED(child_status) {
                kill(process_id, 18);
                fprintf(stderr, "Child process %jd stopped. Continuing.\n", (intmax_t) process_id);
                bg_id = process_id;
                background = true;
              }
            }
            // background process
            else {
              // get process ids in a while loop and add to process array
              while (process_id == 0) {
                process_id = waitpid(process_id, &child_status, WNOHANG);
                for (int i = 0; i < 4; i++) {
                  if (process_arr[i] == -1) {
                    process_arr[i] = process_id;
                  }
                }
              }
              // update global var
              bg_id = process_id;
            }
            // reset vars
            background = false;
            outfile = NULL;
            break;
        }
      } 
    }
  }
  return 0;
}
