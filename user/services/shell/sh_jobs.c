/* Background programs (`prog &`): the shell's table of them, what they
 * print, their end and its notice, and ending one (`kill %n`).
 *
 * A program started with & runs in a job of its own under the shell's job,
 * as a foreground one does, so it ends with the shell: when the shell ends
 * its job is killed with everything in it (init kills a shell it started;
 * a shell run from a shell is in the job its own shell kills).
 *
 * It gets no terminal channel (sh_program.c): no keys, since the shell
 * keeps the keyboard (on a console channel of its own a program could
 * open_keys and take them), no screen to borrow, and Ctrl+C is the
 * foreground program's alone. What it prints comes down an output channel
 * (SR_STDOUT) which the shell copies to the screen whenever it waits: at
 * the prompt, for a foreground program, in a sleep. When the shell is busy
 * elsewhere libos's printf waits for room, so nothing is lost: the
 * program waits.
 *
 * At most SH_MAX_JOBS at once. Each costs the shell three handles (its
 * process, its job, its output channel) and nothing from init, whose spawn
 * of the shell already hands over the most extras a spawn takes. A program
 * that ended keeps its number until its notice ("[2] done: jamjar (exit
 * 0)") is printed: at the next prompt, or by `jobs`. */
#include "sh_core.h"

static struct sh_job jobs[SH_MAX_JOBS];   /* jobs[i] is number i + 1 */
static bool at_prompt;     /* this poll's output goes over the prompt's line */
static bool wrote;         /* this poll wrote something */
static bool mid_line;      /* the last background output didn't end its line */

bool sh_jobs_full(void)
{
    for (unsigned i = 0; i < SH_MAX_JOBS; i++)
        if (!jobs[i].n)
            return false;
    return true;
}

bool sh_jobs_running(void)
{
    for (unsigned i = 0; i < SH_MAX_JOBS; i++)
        if (jobs[i].n && !jobs[i].ended)
            return true;
    return false;
}

const struct sh_job *sh_job_at(unsigned i)
{
    return i < SH_MAX_JOBS && jobs[i].n ? &jobs[i] : NULL;
}

unsigned sh_jobs_add(const char *path, int argc, char **argv, handle_t proc, handle_t job,
                     handle_t out_r)
{
    unsigned i;
    for (i = 0; i < SH_MAX_JOBS && jobs[i].n; i++)
        ;
    if (i == SH_MAX_JOBS)
        return 0;   /* the caller checked sh_jobs_full first */
    struct sh_job *j = &jobs[i];
    *j = (struct sh_job){ .n = i + 1, .proc = proc, .job = job, .out_r = out_r };
    snprintf(j->name, sizeof(j->name), "%s", sh_basename(path));
    struct process_info info;
    if (jam_process_get_info(proc, &info) == OK)
        j->pid = info.koid;
    size_t used = 0;
    for (int k = 1; k < argc && used + 1 < sizeof(j->args); k++) {
        int w = snprintf(j->args + used, sizeof(j->args) - used, "%s%s", used ? " " : "",
                         argv[k]);
        used = w < 0 ? used : used + (size_t)w;
    }
    return j->n;
}

/* Background output to the screen: over the prompt's line (wiped first)
 * when the shell is waiting at the prompt. */
static void bg_put(const char *s, size_t n)
{
    if (!n)
        return;
    if (at_prompt && !wrote)
        sh_console_write("\r\033[K", 4);
    wrote = true;
    sh_console_write(s, n);
    mid_line = s[n - 1] != '\n';
}

/* It ended (*info): the rest of its output, then whatever it left running
 * goes, and its handles; its number stays until the notice. */
static void finish(struct sh_job *j, const struct process_info *info)
{
    for (unsigned guard = 0; guard < 64 && sh_copy_output(j->out_r, true, bg_put); guard++)
        ;
    struct job_info ji;
    /* An empty job is left alone (a kill is a line in the log). */
    if (jam_job_get_info(j->job, &ji) != OK || ji.used[JOB_LIMIT_THREADS])
        jam_job_kill(j->job);
    jam_handle_close(j->out_r);
    jam_handle_close(j->proc);
    jam_handle_close(j->job);
    j->out_r = j->proc = j->job = HANDLE_INVALID;
    j->ended = true;
    j->killed = info->killed != 0;
    j->code = info->exit_code;
}

bool sh_jobs_poll(bool prompt)
{
    at_prompt = prompt;
    wrote = false;
    for (unsigned i = 0; i < SH_MAX_JOBS; i++) {
        struct sh_job *j = &jobs[i];
        if (!j->n || j->ended)
            continue;
        (void)sh_copy_output(j->out_r, true, bg_put);   /* one round; the rest next time */
        struct process_info info;
        if (spawn_wait(j->proc, 0, &info) == OK)
            finish(j, &info);
    }
    if (wrote && mid_line) {
        sh_console_write("\n", 1);
        mid_line = false;
    }
    return wrote;
}

void sh_job_say_end(const struct sh_job *j)
{
    if (j->killed)
        sh_tty("[%u] killed: %s\n", j->n, j->name);
    else
        sh_tty("[%u] done: %s (exit %ld)\n", j->n, j->name, (long)j->code);
}

void sh_job_forget(unsigned n)
{
    if (n >= 1 && n <= SH_MAX_JOBS && jobs[n - 1].ended)
        jobs[n - 1] = (struct sh_job){ 0 };
}

void sh_jobs_report(void)
{
    sh_jobs_poll(false);
    for (unsigned i = 0; i < SH_MAX_JOBS; i++)
        if (jobs[i].n && jobs[i].ended) {
            sh_job_say_end(&jobs[i]);
            sh_job_forget(jobs[i].n);
        }
}

unsigned sh_job_by_pid(uint64_t pid)
{
    for (unsigned i = 0; i < SH_MAX_JOBS; i++)
        if (jobs[i].n && !jobs[i].ended && jobs[i].pid == pid)
            return jobs[i].n;
    return 0;
}

status_t sh_job_kill(unsigned n)
{
    if (n < 1 || n > SH_MAX_JOBS || !jobs[n - 1].n)
        return ERR_NOT_FOUND;
    struct sh_job *j = &jobs[n - 1];
    if (j->ended)
        return ERR_BAD_STATE;
    status_t st = jam_job_kill(j->job);   /* returns once all of it is dead */
    struct process_info info;
    if (st == OK)
        st = spawn_wait(j->proc, 5 * NS_PER_S, &info);
    if (st != OK)
        return st;
    finish(j, &info);
    return OK;
}
