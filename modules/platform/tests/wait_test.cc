// platform_wait and platform_wake with no window (docs/PRDs/Engine-Slice-12-PRD.md, KR26):
// a wake before the wait ends it at once, a wait with nothing to end it lasts its time,
// and a wake from another thread ends it about when it is sent. Each counted. A socket
// given to watch wakes it when something arrives, again while that is unread, and no more
// once it is forgotten.
#include "korvid/platform/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/os_thread.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include "korvid/platform/net.h"
#include "korvid/platform/process.h"
#include "korvid/core/format.h"

namespace {

using namespace kv;

struct Waker { Engine* e; f64 after; f64 sent; };

void wake_later(void* arg) {
    Waker& w = *(Waker*)arg;
    time_sleep(w.after);
    w.sent = time_now();
    platform_wake(*w.e);
}

} // namespace

void test_wait(kv::TestCtx& c) {
    using namespace kv;
    c.begin("platform wait");
    const ModuleDesc* const list[] = {&platform_module};
    Engine e;
    KV_CHECK(c, engine_init(e, list));

    // A wake that came first: the wait ends without waiting, and takes it.
    platform_wake(e);
    f64 t0 = time_now();
    KV_CHECK(c, platform_wait(e, 1.0) == PlatformWoken::Wake && time_now() - t0 < 0.1);
    // Taken: the next wait lasts its time.
    t0 = time_now();
    KV_CHECK(c, platform_wait(e, 0.05) == PlatformWoken::Time);
    f64 took = time_now() - t0;
    KV_CHECK(c, took >= 0.049 && took < 0.5);
    // No time: no wait, and a wake stays for the next.
    platform_wake(e);
    KV_CHECK(c, platform_wait(e, 0.0) == PlatformWoken::Time);
    KV_CHECK(c, platform_wait(e, 1.0) == PlatformWoken::Wake);

    // From another thread, mid-wait.
    Waker w{&e, 0.05, 0.0};
    OsThread th;
    KV_CHECK(c, os_thread_start(&th, wake_later, &w));
    t0 = time_now();
    PlatformWoken why = platform_wait(e, 5.0);
    f64 woke = time_now();
    os_thread_join(&th);
    KV_CHECK(c, why == PlatformWoken::Wake && woke - t0 < 2.0 && woke >= w.sent);
    KV_INFO("test", "a wake from another thread ended the wait %.3f ms after it was sent", (woke - w.sent) * 1e3);

    PlatformWaits n = platform_waits(e);
    KV_CHECK(c, n.waits == 4 && n.woken == 3 && n.timed == 1 && n.window == 0);

    // A socket given to watch (PlatformWatch): what arrives on it ends the wait.
    {
        NetListener l;
        NetSocket client, server;
        KV_CHECK(c, l.open(0) && net_connect_local(l.port, &client));
        for (u32 i = 0; i < 200 && !os_handle_open(server.fd); ++i) {
            Handle fd = l.accept();
            if (os_handle_open(fd)) server.adopt(fd); else time_sleep(0.005);
        }
        KV_CHECK(c, os_handle_open(server.fd));
        int owner = 0;
        {
            PlatformWatch pw(e);
            pw.add(l);
            pw.add(server);
            KV_CHECK(c, pw.count == 2 && pw.due == 0.0); // both watched, and no time asked for
            platform_watch(e, &owner, pw);
        }
        KV_CHECK(c, platform_wait(e, 0.1) == PlatformWoken::Time); // nothing sent
        client.write_str("x");
        client.flush();
        t0 = time_now();
        KV_CHECK(c, platform_wait(e, 5.0) == PlatformWoken::Wake && time_now() - t0 < 2.0);
        // Not read: watched again at the next wait, which it ends too.
        KV_CHECK(c, platform_wait(e, 5.0) == PlatformWoken::Wake);
        // Read: the next wait lasts its time.
        char buf[8];
        KV_CHECK(c, server.read(buf, sizeof buf) == 1);
        KV_CHECK(c, platform_wait(e, 0.1) == PlatformWoken::Time);
        // Forgotten: what arrives wakes nothing.
        platform_unwatch(e, &owner);
        client.write_str("y");
        client.flush();
        KV_CHECK(c, platform_wait(e, 0.1) == PlatformWoken::Time);
        // A handle knows what it names, and a watch takes each kind as this OS can. A
        // real file (a lock's) and a real child's pipes: every handle here is one the
        // platform issued, as any holder's is.
        char lock_dir[256], lock_path[400];
        KV_CHECK(c, io_make_temp_dir("kv-wait-kinds", lock_dir, sizeof lock_dir));
        format(lock_path, sizeof lock_path, "%s/a.lock", lock_dir);
        Handle none, file;
        KV_CHECK(c, io_lock_file(lock_path, "wait_test", &file));
        KV_CHECK(c, !os_handle_open(none) && os_handle_kind(none) == OsKind::None);
        KV_CHECK(c, os_handle_open(file) && os_handle_kind(file) == OsKind::File && file != server.fd);
        KV_CHECK(c, os_handle_kind(server.fd) == OsKind::Socket && os_handle_kind(l.fd) == OsKind::Socket);
        Process child;
#if defined(_WIN32)
        KV_CHECK(c, child.shell("ping -n 3 127.0.0.1 >nul"));
#else
        KV_CHECK(c, child.shell("sleep 2"));
#endif
        KV_CHECK(c, os_handle_kind(child.out_fd) == OsKind::Pipe && os_handle_kind(child.err_fd) == OsKind::Pipe);
        {
            PlatformWatch pw(e);
            pw.add(none);                                  // nothing: nothing asked
            KV_CHECK(c, pw.count == 0 && pw.due == 0.0);
            pw.add(file);                                  // a mistake on any OS: one error line, not watched
            KV_CHECK(c, pw.count == 0 && pw.due == 0.0);
            f64 before = time_now();
            pw.add(child);                                 // its pipes watched (POSIX) or asked again soon (Windows), and its exit watched
#if defined(_WIN32)
            KV_CHECK(c, pw.count == 1 && pw.due > 0.0);    // the exit; the pipes soon
#else
            KV_CHECK(c, pw.due == 0.0 && (pw.count == 3 || (pw.count == 2 && !os_handle_open(child.process)))); // no exit handle: a kernel before 5.3
#endif
            KV_CHECK(c, pw.due == 0.0 || (pw.due >= before && pw.due <= time_now() + kPlatformSoon));
            pw.at(100.0);
            pw.at(50.0);
            pw.at(75.0);
            KV_CHECK(c, pw.due > 0.0 && pw.due <= 50.0);    // the earliest stands
        }
        // The socket beside what could not be watched is watched still.
        {
            PlatformWatch pw(e);
            pw.add(file);
            pw.add(server);
            KV_CHECK(c, pw.count == 1);
            platform_watch(e, &owner, pw);
        }
        client.write_str("z");
        client.flush();
        KV_CHECK(c, platform_wait(e, 5.0) == PlatformWoken::Wake);
        KV_CHECK(c, server.read(buf, sizeof buf) > 0);
        // What is written and not yet sent is asked about soon: nothing readable would say.
        server.write_str("queued");
        {
            PlatformWatch pw(e);
            pw.add(server);
            KV_CHECK(c, pw.count == 1 && pw.due > 0.0);
        }
        server.flush();
        platform_unwatch(e, &owner);

        // A closed handle is stale, everywhere at once: every copy stops naming anything,
        // and a watch that still lists it leaves it out instead of polling a dead number.
        Handle pipe_copy = child.out_fd, file_copy = file;
        child.close();
        io_unlock_file(file);
        KV_CHECK(c, !os_handle_open(pipe_copy) && os_handle_kind(pipe_copy) == OsKind::None);
        KV_CHECK(c, !os_handle_open(file_copy) && !os_handle_open(file));
        {
            NetListener gone;
            KV_CHECK(c, gone.open(0));
            Handle was = gone.fd;
            PlatformWatch pw(e);
            pw.add(gone);
            KV_CHECK(c, pw.count == 1);
            platform_watch(e, &owner, pw);      // watched...
            gone.close();                       // ...and closed under the watch
            KV_CHECK(c, !os_handle_open(was));
            KV_CHECK(c, platform_wait(e, 0.1) == PlatformWoken::Time); // no wake, no error: it is simply not there
            PlatformWatch again(e);
            again.add(was);                     // a stale handle is nothing to watch
            KV_CHECK(c, again.count == 0 && again.due == 0.0);
            platform_unwatch(e, &owner);
        }
        io_remove_tree(lock_dir);

        // A queue's task wakes the loop as it finishes: no poll for it.
        IoQueue q;
        KV_CHECK(c, q.init(mem_general(), 16, IoBackend::Sync) && q.start_tasks(1));
        {
            PlatformWatch pw(e);
            pw.add(q);
            KV_CHECK(c, pw.count == 0 && pw.due == 0.0); // nothing queued for the pump: no time asked
            platform_watch(e, &owner, pw);
        }
        IoTaskRequest tr;
        tr.fn = [](void*, const IoTaskCtl&) -> i32 { time_sleep(0.02); return 0; };
        Handle task = q.run_task(tr);
        t0 = time_now();
        KV_CHECK(c, !task.is_null() && platform_wait(e, 5.0) == PlatformWoken::Wake && time_now() - t0 < 2.0);
        q.pump();
        IoCompletion done;
        KV_CHECK(c, q.take(task, &done) && done.status == IoStatus::Done);
        q.free();
        platform_unwatch(e, &owner);

        // A child's exit ends the wait by itself, its pipes not watched at all, and is heard
        // once: the next wait, the same watch still stated, lasts its time.
        Process quiet;
#if defined(_WIN32)
        KV_CHECK(c, quiet.shell("ping -n 2 127.0.0.1 >nul")); // about a second
#else
        KV_CHECK(c, quiet.shell("sleep 0.3"));
#endif
        if (os_handle_open(quiet.process) && os_handle_kind(quiet.process) == OsKind::Process) {
            {
                PlatformWatch pw(e);
                pw.add(quiet.process);
                KV_CHECK(c, pw.count == 1 && pw.due == 0.0);
                platform_watch(e, &owner, pw);
            }
            t0 = time_now();
            PlatformWoken ended = platform_wait(e, 10.0);
            f64 heard = time_now() - t0;
            KV_CHECK(c, ended == PlatformWoken::Wake && heard >= 0.2 && heard < 5.0);
            for (u32 i = 0; i < 1000 && quiet.running(); ++i) time_sleep(0.001); // the exit is seen as it is heard
            KV_CHECK(c, !quiet.running() && quiet.code == 0);
            KV_CHECK(c, platform_wait(e, 0.1) == PlatformWoken::Time);
            KV_INFO("test", "a child's exit ended the wait %.3f s in", heard);
        } else {
            KV_INFO("test", "this OS gives no handle for a child's exit: not watched");
        }
        platform_unwatch(e, &owner);
        quiet.close();

        // A standing watch: added once, no waker, nothing stated again. What arrives ends
        // the wait; read, the next wait lasts its time; a stated watch beside it leaves it
        // standing; a socket with output queued asks to be looked at soon; closed, it is let
        // go, and its owner's set is the smaller for it.
        {
            int stands = 0;
            platform_watch_add(e, &stands, server);
            platform_watch_add(e, &stands, server); // twice is once
            client.write_str("s");
            client.flush();
            KV_CHECK(c, platform_wait(e, 5.0) == PlatformWoken::Wake);
            KV_CHECK(c, server.read(buf, sizeof buf) == 1);
            KV_CHECK(c, platform_wait(e, 0.1) == PlatformWoken::Time);
            {
                PlatformWatch pw(e);
                pw.at(time_now() + 60.0);
                platform_watch(e, &stands, pw); // a stated set of nothing: the standing one stays
            }
            client.write_str("t");
            client.flush();
            KV_CHECK(c, platform_wait(e, 5.0) == PlatformWoken::Wake);
            KV_CHECK(c, server.read(buf, sizeof buf) == 1);
            KV_CHECK(c, platform_idle_time(e, 1.0) > kPlatformSoon);
            server.write_str("queued");     // the kernel takes it at the flush; until then, soon
            KV_CHECK(c, platform_idle_time(e, 1.0) <= kPlatformSoon);
            server.flush();
            KV_CHECK(c, server.pending() == 0 && platform_idle_time(e, 1.0) > kPlatformSoon);
            platform_watch_remove(e, &stands, server); // taken back: what arrives wakes nothing
            client.write_str("u");
            client.flush();
            KV_CHECK(c, platform_wait(e, 0.1) == PlatformWoken::Time);
            KV_CHECK(c, server.read(buf, sizeof buf) == 1);
            NetListener lone;
            KV_CHECK(c, lone.open(0));
            platform_watch_add(e, &stands, lone);
            lone.close();                   // closed while standing: let go, no wake, no error
            KV_CHECK(c, platform_idle_time(e, 0.2) > kPlatformSoon);
            KV_CHECK(c, platform_wait(e, 0.1) == PlatformWoken::Time);
            platform_unwatch(e, &stands);
        }
        server.close();
        client.close();
        l.close();
    }
    engine_shutdown(e);
}
