/* completion: shell completion scripts for the commands and their flags,
 * printed, or installed into the shell's start-up files. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cmds.h"
#include "compat.h"

static int run_completion(command *c, int argc, char **argv);

command cmd_completion = {
	.name = "completion",
	.use = "completion [bash|zsh|fish|powershell|install|uninstall]",
	.short_desc = "Generate shell completion scripts",
	.long_desc =
		"Install completions for your shell (from $SHELL, or name it):\n\n"
		"  $ mmdbio completion install\n"
		"  $ mmdbio completion install zsh\n"
		"  $ mmdbio completion uninstall\n\n"
		"Or load them yourself:\n\n"
		"Bash:\n\n"
		"  $ source <(mmdbio completion bash)\n\n"
		"  # To load completions for each session, execute once:\n"
		"  # Linux:\n"
		"  $ mmdbio completion bash > /etc/bash_completion.d/mmdbio\n"
		"  # macOS:\n"
		"  $ mmdbio completion bash > /usr/local/etc/bash_completion.d/mmdbio\n\n"
		"Zsh:\n\n"
		"  $ echo \"autoload -U compinit; compinit\" >> ~/.zshrc\n"
		"  $ mmdbio completion zsh > \"${fpath[1]}/_mmdbio\"\n\n"
		"Fish:\n\n"
		"  $ mmdbio completion fish | source\n"
		"  $ mmdbio completion fish > ~/.config/fish/completions/mmdbio.fish\n\n"
		"PowerShell:\n\n"
		"  PS> mmdbio completion powershell | Out-String | Invoke-Expression\n"
		"  PS> mmdbio completion powershell > mmdbio.ps1",
	.flags = NULL,
	.nflags = 0,
	.run = run_completion,
};

static const char *const SHELL_NAMES[] = {"bash", "zsh", "fish", "powershell", NULL};

/* the program completed */
static const char PROG[] = "mmdbio";

/* ---- what to suggest (mmdbio __complete N WORD...) ----
 *
 * The scripts below ask the program itself: WORDs are the words after the
 * program's name and N is the index of the one being completed (past the
 * end when it is still empty). The answer is one candidate per line, or the
 * line "__files__" (optionally followed by an extension) for file names. */

typedef struct {
	const char *cur;
	size_t curlen;
	const char *prefix; /* "--format=" when completing after the = */
	int n;
} answer;

static void offer(answer *a, const char *word)
{
	if (strncmp(word, a->cur, a->curlen) != 0)
		return;
	printf("%s%s\n", a->prefix ? a->prefix : "", word);
	a->n++;
}

static void offer_list(answer *a, const char *const *words)
{
	for (; *words; words++)
		offer(a, *words);
}

static const flag *find_flag(const command *c, const char *word)
{
	if (word[0] != '-')
		return NULL;
	if (word[1] == '-') {
		size_t n = strcspn(word + 2, "=");
		for (int i = 0; i < c->nflags; i++)
			if (strlen(c->flags[i].name) == n && strncmp(c->flags[i].name, word + 2, n) == 0)
				return &c->flags[i];
		return NULL;
	}
	if (word[1] && !word[2])
		for (int i = 0; i < c->nflags; i++)
			if (c->flags[i].shorthand == word[1])
				return &c->flags[i];
	return NULL;
}

/* the values a flag takes, or file names */
static void flag_values(answer *a, const command *c, const flag *f)
{
	static const char *const read_formats[] = {"json", "jsonl", "json-pretty", "csv", "tsv", NULL};
	static const char *const export_formats[] = {"json", "jsonl", "csv", "tsv", "cidr", NULL};
	static const char *const import_formats[] = {"auto", "csv", "tsv", "json", "jsonl", NULL};
	static const char *const text_json[] = {"text", "json", NULL};
	static const char *const merges[] = {"none", "keep", "toplevel", "recurse", NULL};
	static const char *const sizes[] = {"auto", "24", "28", "32", NULL};
	static const char *const record_sizes[] = {"0", "24", "28", "32", NULL};
	static const char *const families[] = {"4", "6", NULL};
	static const char *const headers[] = {"auto", "true", "false", NULL};
	static const char *const delimiters[] = {"comma", "tab", "semicolon", "pipe", "space", NULL};
	static const char *const types[] = {"string", "uint16", "uint32", "uint64", "uint128", "int32", "double",
					    "float", "boolean", "array", "auto", NULL};
	const char *n = f->name;
	if (strcmp(n, "format") == 0)
		offer_list(a, c == &cmd_read ? read_formats : c == &cmd_export ? export_formats
			      : c == &cmd_import ? import_formats : text_json);
	else if (strcmp(n, "merge") == 0)
		offer_list(a, merges);
	else if (strcmp(n, "size") == 0)
		offer_list(a, sizes);
	else if (strcmp(n, "record-size") == 0)
		offer_list(a, record_sizes);
	else if (strcmp(n, "ip") == 0 && c == &cmd_import)
		offer_list(a, families);
	else if (strcmp(n, "family") == 0)
		offer_list(a, families);
	else if (strcmp(n, "header") == 0)
		offer_list(a, headers);
	else if (strcmp(n, "delimiter") == 0)
		offer_list(a, delimiters);
	else if (strcmp(n, "type") == 0) {
		/* field=type: the types after the = */
		const char *eq = memchr(a->cur, '=', a->curlen);
		if (eq) {
			buf_t pre = {0};
			buf_printf(&pre, "%s%.*s", a->prefix ? a->prefix : "", (int)(eq + 1 - a->cur), a->cur);
			answer b = {.cur = eq + 1, .curlen = a->curlen - (size_t)(eq + 1 - a->cur), .prefix = buf_cstr(&pre)};
			offer_list(&b, types);
			a->n += b.n;
			buf_free(&pre);
		}
	} else if (strcmp(n, "db") == 0 || strcmp(n, "old") == 0 || strcmp(n, "new") == 0)
		printf("__files__ .mmdb\n");
	else if (strcmp(n, "in") == 0 || strcmp(n, "input") == 0 || strcmp(n, "out") == 0 || strcmp(n, "config") == 0 ||
		 strcmp(n, "lookup") == 0 || strcmp(n, "tmp-dir") == 0)
		printf("__files__\n");
}

static void flag_names(answer *a, const command *c)
{
	for (int i = 0; i < c->nflags; i++) {
		char w[96];
		snprintf(w, sizeof w, "--%s", c->flags[i].name);
		offer(a, w);
	}
	offer(a, "--help");
	offer(a, "--nocolor");
}

/* positional words: where a command takes a database (read's are IPs and
 * files as well, and import's are its input and output) */
static bool positional_db(const command *c, int npos)
{
	if (c == &cmd_read || c == &cmd_import)
		return false;
	if (c == &cmd_diff)
		return npos < 2;
	return npos == 0 || c == &cmd_shrink;
}

static int complete_words(int argc, char **argv)
{
	if (argc < 1)
		return 0;
	int at = atoi(argv[0]);
	char **w = argv + 1;
	int nw = argc - 1;
	if (at < 0)
		at = 0;
	if (at > nw)
		at = nw;
	answer a = {.cur = at < nw ? w[at] : ""};
	a.curlen = strlen(a.cur);
	if (at == 0) {
		for (int i = 0; i < ncommands; i++)
			offer(&a, all_commands[i]->name);
		static const char *const extra[] = {"help", "--help", "--version", "--nocolor", NULL};
		offer_list(&a, extra);
		return 0;
	}
	const command *c = NULL;
	for (int i = 0; i < ncommands; i++)
		if (strcmp(all_commands[i]->name, w[0]) == 0)
			c = all_commands[i];
	if (!c && strcmp(w[0], "help") == 0) {
		if (at == 1)
			for (int i = 0; i < ncommands; i++)
				offer(&a, all_commands[i]->name);
		return 0;
	}
	if (!c)
		return 0;
	if (c == &cmd_completion) {
		int k = at - 1;
		const char *sub = k >= 1 ? w[1] : NULL;
		if (k == 0) {
			offer_list(&a, SHELL_NAMES);
			offer(&a, "install");
			offer(&a, "uninstall");
		} else if (k == 1 && sub && (strcmp(sub, "install") == 0 || strcmp(sub, "uninstall") == 0)) {
			offer_list(&a, SHELL_NAMES);
		}
		return 0;
	}
	/* --flag=value */
	if (a.cur[0] == '-' && a.cur[1] == '-' && strchr(a.cur, '=')) {
		const flag *f = find_flag(c, a.cur);
		if (f && f->type != F_BOOL) {
			const char *eq = strchr(a.cur, '=');
			buf_t pre = {0};
			buf_printf(&pre, "%.*s", (int)(eq + 1 - a.cur), a.cur);
			answer b = {.cur = eq + 1, .curlen = strlen(eq + 1), .prefix = buf_cstr(&pre)};
			flag_values(&b, c, f);
			buf_free(&pre);
		}
		return 0;
	}
	/* the value of the flag before */
	if (at > 1) {
		const flag *f = find_flag(c, w[at - 1]);
		if (f && f->type != F_BOOL && !strchr(w[at - 1], '=')) {
			flag_values(&a, c, f);
			return 0;
		}
	}
	if (a.cur[0] == '-') {
		flag_names(&a, c);
		return 0;
	}
	/* a positional word: count the ones before it */
	int npos = 0;
	for (int i = 1; i < at; i++) {
		const flag *f = find_flag(c, w[i]);
		if (w[i][0] == '-') {
			if (f && f->type != F_BOOL && !strchr(w[i], '='))
				i++;
			continue;
		}
		npos++;
	}
	printf(positional_db(c, npos) ? "__files__ .mmdb\n" : "__files__\n");
	return 0;
}

/* ---- the scripts: each asks the program ---- */

static void bash(FILE *o)
{
	const char *p = PROG;
	fprintf(o,
		"# bash completion for %s\n"
		"_%s() {\n"
		"  local cur=\"${COMP_WORDS[COMP_CWORD]}\" out\n"
		"  out=$(\"${COMP_WORDS[0]}\" __complete \"$((COMP_CWORD - 1))\" \"${COMP_WORDS[@]:1}\" 2>/dev/null)\n"
		"  case \"$out\" in\n"
		"    __files__\\ *) COMPREPLY=($(compgen -f -X \"!*${out#__files__ }\" -- \"$cur\") $(compgen -d -- \"$cur\"))\n"
		"      compopt -o filenames 2>/dev/null ;;\n"
		"    __files__*) COMPREPLY=($(compgen -f -- \"$cur\")); compopt -o filenames 2>/dev/null ;;\n"
		"    *) local IFS=$'\\n'; COMPREPLY=($out) ;;\n"
		"  esac\n"
		"}\n"
		"complete -o default -F _%s %s\n",
		p, p, p, p);
}

static void zsh(FILE *o)
{
	const char *p = PROG;
	fprintf(o,
		"#compdef %s\n"
		"_%s() {\n"
		"  local out\n"
		"  out=$(\"${words[1]}\" __complete $((CURRENT - 2)) \"${(@)words[2,-1]}\" 2>/dev/null)\n"
		"  if [[ $out == '__files__ '* ]]; then\n"
		"    _files -g \"*${out#__files__ }\"\n"
		"  elif [[ $out == __files__* ]]; then\n"
		"    _files\n"
		"  else\n"
		"    local -a cands\n"
		"    cands=(${(f)out})\n"
		"    compadd -Q -- $cands\n"
		"  fi\n"
		"}\n"
		"compdef _%s %s\n",
		p, p, p, p);
}

static void fish(FILE *o)
{
	const char *p = PROG;
	fprintf(o,
		"# fish completion for %s\n"
		"function __%s_complete\n"
		"  set -l words (commandline -opc) (commandline -ct)\n"
		"  set -l prog $words[1]\n"
		"  set -e words[1]\n"
		"  set -l out ($prog __complete (math (count $words) - 1) $words 2>/dev/null)\n"
		"  if string match -q '__files__ *' -- \"$out[1]\"\n"
		"    __fish_complete_suffix (string sub -s 11 -- $out[1])\n"
		"  else if string match -q '__files__*' -- \"$out[1]\"\n"
		"    __fish_complete_path (commandline -ct)\n"
		"  else\n"
		"    printf '%%s\\n' $out\n"
		"  end\n"
		"end\n"
		"complete -c %s -f -a '(__%s_complete)'\n",
		p, p, p, p);
}

static void powershell(FILE *o)
{
	const char *p = PROG;
	fprintf(o,
		"# powershell completion for %s\n"
		"Register-ArgumentCompleter -Native -CommandName '%s' -ScriptBlock {\n"
		"  param($wordToComplete, $commandAst, $cursorPosition)\n"
		"  $words = @($commandAst.CommandElements | Select-Object -Skip 1 | ForEach-Object { $_.ToString() })\n"
		"  $at = if ($wordToComplete -eq '') { $words.Count } else { $words.Count - 1 }\n"
		"  $out = @(& $commandAst.CommandElements[0].ToString() __complete $at @words 2>$null)\n"
		"  if ($out.Count -gt 0 -and $out[0] -like '__files__*') { return }\n"
		"  $out | ForEach-Object {\n"
		"    [System.Management.Automation.CompletionResult]::new($_, $_, 'ParameterValue', $_)\n"
		"  }\n"
		"}\n",
		p, p);
}

static void (*generator(const char *shell))(FILE *)
{
	if (strcmp(shell, "bash") == 0)
		return bash;
	if (strcmp(shell, "zsh") == 0)
		return zsh;
	if (strcmp(shell, "fish") == 0)
		return fish;
	if (strcmp(shell, "powershell") == 0 || strcmp(shell, "pwsh") == 0)
		return powershell;
	return NULL;
}

/* ---- install ---- */

static int read_text(const char *path, buf_t *out)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return errno == ENOENT ? 0 : -1;
	char chunk[4096];
	size_t n;
	while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
		buf_append(out, chunk, n);
	fclose(f);
	return 0;
}

/* the start-up file's text without our block (begin..end lines) */
static bool strip_block(buf_t *text, const char *begin, const char *end)
{
	const char *t = buf_cstr(text);
	const char *b = strstr(t, begin);
	if (!b)
		return false;
	const char *e = strstr(b, end);
	if (!e)
		return false;
	e += strlen(end);
	if (*e == '\n')
		e++;
	size_t from = (size_t)(b - t), to = (size_t)(e - t);
	memmove(text->p + from, text->p + to, text->len - to);
	text->len -= to - from;
	return true;
}

static int write_text(const char *path, const buf_t *text)
{
	make_parent_dirs(path);
	if (write_file(path, text->p, text->len) < 0) {
		fprintf(stderr, "Error: %s\n", err_msg());
		return -1;
	}
	return 0;
}

static int install(const char *shell, bool remove)
{
	const char *p = PROG;
	const char *home = getenv("HOME");
#ifdef _WIN32
	if (!home || !*home)
		home = getenv("USERPROFILE");
#endif
	if (!home || !*home) {
		fprintf(stderr, "Error: $HOME is not set\n");
		return 1;
	}
	void (*gen)(FILE *) = generator(shell);
	if (!gen) {
		fprintf(stderr, "Error: unknown shell \"%s\": use bash, zsh, fish or powershell\n", shell);
		return 1;
	}
	buf_t script = {0}, rc = {0}, text = {0}, begin = {0}, end = {0};
	const char *data = getenv("XDG_DATA_HOME");
	if (strcmp(shell, "fish") == 0) {
		const char *cfg = getenv("XDG_CONFIG_HOME");
		if (cfg && *cfg)
			buf_printf(&script, "%s/fish/completions/%s.fish", cfg, p);
		else
			buf_printf(&script, "%s/.config/fish/completions/%s.fish", home, p);
	} else {
		if (data && *data)
			buf_printf(&script, "%s/%s/completion.%s", data, p, shell[0] == 'p' ? "ps1" : shell);
		else
			buf_printf(&script, "%s/.local/share/%s/completion.%s", home, p, shell[0] == 'p' ? "ps1" : shell);
		if (strcmp(shell, "bash") == 0)
			buf_printf(&rc, "%s/.bashrc", home);
		else if (strcmp(shell, "zsh") == 0) {
			const char *zd = getenv("ZDOTDIR");
			buf_printf(&rc, "%s/.zshrc", zd && *zd ? zd : home);
		} else {
#ifdef _WIN32
			buf_printf(&rc, "%s/Documents/PowerShell/Microsoft.PowerShell_profile.ps1", home);
#else
			buf_printf(&rc, "%s/.config/powershell/Microsoft.PowerShell_profile.ps1", home);
#endif
		}
	}
	buf_printf(&begin, "# >>> %s completion >>>", p);
	buf_printf(&end, "# <<< %s completion <<<", p);
	int status = 0;
	const char *sp = buf_cstr(&script);
	if (remove) {
		bool any = unlink(sp) == 0;
		if (rc.len && read_text(buf_cstr(&rc), &text) == 0 && strip_block(&text, buf_cstr(&begin), buf_cstr(&end))) {
			if (write_text(buf_cstr(&rc), &text) < 0)
				status = 1;
			else
				printf("removed the %s completion lines from %s\n", p, buf_cstr(&rc));
			any = true;
		}
		if (any)
			printf("removed %s\n", sp);
		else
			printf("no %s completion was installed for %s\n", shell, p);
		goto done;
	}
	make_parent_dirs(sp);
	FILE *f = fopen(sp, "wb");
	if (!f) {
		fprintf(stderr, "Error: create %s: %s\n", sp, go_strerror(errno));
		status = 1;
		goto done;
	}
	gen(f);
	if (fclose(f) != 0) {
		fprintf(stderr, "Error: write %s: %s\n", sp, go_strerror(errno));
		status = 1;
		goto done;
	}
	printf("wrote %s\n", sp);
	if (rc.len) {
		if (read_text(buf_cstr(&rc), &text) < 0) {
			fprintf(stderr, "Error: read %s: %s\n", buf_cstr(&rc), go_strerror(errno));
			status = 1;
			goto done;
		}
		strip_block(&text, buf_cstr(&begin), buf_cstr(&end));
		if (text.len && text.p[text.len - 1] != '\n')
			buf_push(&text, '\n');
		buf_printf(&text, "%s\n", buf_cstr(&begin));
		if (strcmp(shell, "zsh") == 0)
			buf_printf(&text, "autoload -Uz compinit && { (( ${+functions[compdef]} )) || compinit; }\n");
		if (shell[0] == 'p')
			buf_printf(&text, "if (Test-Path '%s') { . '%s' }\n", sp, sp);
		else
			buf_printf(&text, "[ -f \"%s\" ] && . \"%s\"\n", sp, sp);
		buf_printf(&text, "%s\n", buf_cstr(&end));
		if (write_text(buf_cstr(&rc), &text) < 0) {
			status = 1;
			goto done;
		}
		printf("loaded from %s\n", buf_cstr(&rc));
	}
	printf("%s completion for %s is installed; open a new shell to use it\n", shell, p);
done:
	buf_free(&script);
	buf_free(&rc);
	buf_free(&text);
	buf_free(&begin);
	buf_free(&end);
	return status;
}

static const char *login_shell(void)
{
	const char *sh = getenv("SHELL");
	if (!sh || !*sh) {
#ifdef _WIN32
		return "powershell";
#else
		return NULL;
#endif
	}
	const char *b = strrchr(sh, '/');
	return b ? b + 1 : sh;
}

int completion_answer(int argc, char **argv) { return complete_words(argc, argv); }

static int run_completion(command *c, int argc, char **argv)
{
	if (argc >= 1 && (strcmp(argv[0], "install") == 0 || strcmp(argv[0], "uninstall") == 0)) {
		if (argc > 2) {
			fprintf(stderr, "Error: accepts at most 2 arg(s), received %d\n", argc);
			return 1;
		}
		const char *shell = argc == 2 ? argv[1] : login_shell();
		if (!shell) {
			fprintf(stderr, "Error: $SHELL is not set: name the shell (bash, zsh, fish or powershell)\n");
			return 1;
		}
		return install(shell, argv[0][0] == 'u');
	}
	if (argc != 1) {
		fprintf(stderr, "Error: accepts 1 arg(s), received %d\n", argc);
		command_help(c, stderr);
		return 1;
	}
	void (*gen)(FILE *) = generator(argv[0]);
	if (!gen || strcmp(argv[0], "pwsh") == 0) {
		fprintf(stderr, "Error: invalid argument \"%s\" for \"%s completion\"\n", argv[0], PROG);
		command_help(c, stderr);
		return 1;
	}
	gen(stdout);
	return 0;
}
