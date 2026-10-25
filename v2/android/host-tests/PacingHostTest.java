import org.midibottleneck.core.PlayerBridge;

/**
 * Tests of the adaptive refresh rule (PlayerBridge.RefreshPacing) on a plain JVM with a simulated display clock: cheap updates
 * stay at the screen's rate, costly ones step down one tier at a time and come back, a rate cap keeps its average, frames
 * missed while an update runs are counted and skipped (never queued), playback lag backs off only with one or two processors.
 * Run through RouteHostTest.main.
 */
final class PacingHostTest {
    static int checks, failures;

    static void check(boolean ok, String what) {
        checks++;
        if (!ok) { failures++; System.out.println("FAIL: " + what); }
    }

    static final long SECOND = 1000000000L;

    /** A simulated screen: frames at hz, an update costing costNs (all moving, none in the statistics block) when it runs. */
    static final class Sim {
        final PlayerBridge.RefreshPacing pacing;
        final long frameNs;
        long now = SECOND;               // frame time of the next callback
        long frames, updates, maxTierStep;
        int lastTier;
        Sim(int processors, double hz) {
            pacing = new PlayerBridge.RefreshPacing(processors, hz);
            frameNs = Math.round(1e9 / hz);
            pacing.restart();
        }
        /** Runs for seconds of simulated time. Each update takes costNs and the playback lag is lagUs. */
        void run(double seconds, long costNs, long lagUs) {
            final long end = now + Math.round(seconds * SECOND);
            while (now < end) {
                frames++;
                final boolean run = pacing.onFrame(now);
                long next = now + frameNs;
                if (run) {
                    updates++;
                    pacing.onUpdateDone(costNs * 3 / 4, costNs / 4, now, lagUs);
                    if (costNs > frameNs) next = now + ((costNs + frameNs - 1) / frameNs) * frameNs;   // re-armed after it ended
                }
                final int tier = pacing.tier();
                maxTierStep = Math.max(maxTierStep, Math.abs(tier - lastTier));
                lastTier = tier;
                now = next;
            }
        }
    }

    static void testCheapStaysFull() {
        Sim s = new Sim(8, 60);
        s.run(10, 1500000L, 0);
        check(s.pacing.tier() == 0, "cheap updates stay at tier 0");
        check(s.updates == s.frames, "cheap updates run on every display frame (" + s.updates + "/" + s.frames + ")");
        check(s.pacing.tierRate() == 60, "tier 0 rate is the screen's (60)");
        check(!s.pacing.halfStatistics(), "tier 0 draws all statistics cells");
    }

    static void testCostlyStepsDownOneAtATime() {
        Sim s = new Sim(8, 60);
        s.run(1.0, 12000000L, 0);   // 12 ms per frame is 72 % of the thread: far over the 25 % budget
        check(s.pacing.tier() >= 1, "a costly update steps down");
        s.run(10, 12000000L, 0);
        check(s.maxTierStep <= 1, "never more than one tier per decision (" + s.maxTierStep + ")");
        check(s.pacing.tier() >= 3, "a persistent cost keeps stepping down (tier " + s.pacing.tier() + ")");
        check(s.pacing.tierRate() < 60, "lower tiers lower the rate");
        check(s.pacing.summary().contains("tier"), "summary text exists");
    }

    static void testBackUp() {
        Sim s = new Sim(8, 60);
        s.run(5, 12000000L, 0);
        final int low = s.pacing.tier();
        check(low >= 2, "went down first (tier " + low + ")");
        s.run(30, 1000000L, 0);
        check(s.pacing.tier() == 0, "cheap updates bring it back to tier 0 (tier " + s.pacing.tier() + ")");
    }

    static void testCapAverage() {
        Sim s = new Sim(8, 90);                // 90 Hz screen: rungs 60, 30, 15, 10
        // force the tier by cost until a 60 cap is reached, then measure the average rate with cheap updates held there
        s.run(3, 9000000L, 0);                 // 9 ms of 11.1 ms frames: far over
        int guard = 0;
        while (s.pacing.tierRate() > 60 && guard++ < 100) s.run(0.5, 9000000L, 0);
        check(s.pacing.tierRate() <= 60, "reached a capped tier on a 90 Hz screen (rate " + s.pacing.tierRate() + ")");
        check(s.pacing.halfStatistics(), "capped tiers also draw half the statistics");
        final int rate = s.pacing.tierRate();
        final long updatesBefore = s.updates, framesBefore = s.frames;
        // few cheap windows: the tier stays (calm needs 6 windows = 3 s); measure over 2 s
        s.run(2, 200000L, 0);
        final double seconds = (s.frames - framesBefore) / 90.0;
        final double perSecond = (s.updates - updatesBefore) / seconds;
        check(Math.abs(perSecond - rate) <= rate * 0.08 + 1, "a " + rate + " cap on 90 Hz averages " + rate + " per second, got " + perSecond);
    }

    static void testLateFramesAreCounted() {
        Sim s = new Sim(8, 60);
        s.run(2, 40000000L, 0);                // each update takes 40 ms: three display frames per update
        check(s.updates < s.frames, "frames pass while an update runs (updates " + s.updates + ", frames " + s.frames + ")");
        check(s.pacing.summary().contains("late frames"), "late frames are reported");
        // no backlog: never more updates than callbacks, and a callback is simulated only once the previous update ended
        check(s.updates <= s.frames, "never more updates than display callbacks");
        check(!s.pacing.summary().contains("late frames 0,"), "late frames counted when callbacks arrive 3 frames apart");
    }

    static void testBehindOnlyWithFewProcessors() {
        Sim few = new Sim(2, 60);
        few.run(3, 500000L, 30000L);           // cheap, but playback is 30 ms late
        check(few.pacing.tier() >= 1, "with 2 processors a late playback steps the display down");
        Sim many = new Sim(8, 60);
        many.run(3, 500000L, 30000L);
        check(many.pacing.tier() == 0, "with 8 processors playback lag does not change the display");
    }

    static void testLadder() {
        Sim s60 = new Sim(2, 60);
        s60.run(60, 500000L, 30000L);          // forced all the way down
        check(s60.pacing.tier() == 4, "60 Hz ladder: half statistics, then 30, 15, 10 (4 steps), got " + s60.pacing.tier());
        check(s60.pacing.tierRate() == 10, "lowest rung is 10 per second, got " + s60.pacing.tierRate());
        Sim s120 = new Sim(2, 120);
        s120.run(60, 500000L, 30000L);
        check(s120.pacing.tier() == 5, "120 Hz ladder has 60, 30, 15, 10 below the screen, got " + s120.pacing.tier());
    }

    static void testBudgets() {
        check(PlayerBridge.RefreshPacing.displayBudgetForProcessors(1) == 0.08, "budget 1 processor");
        check(PlayerBridge.RefreshPacing.displayBudgetForProcessors(2) == 0.12, "budget 2 processors");
        check(PlayerBridge.RefreshPacing.displayBudgetForProcessors(8) == 0.25, "budget 3 or more processors");
    }

    static void run() {
        checks = 0;
        failures = 0;
        testCheapStaysFull();
        testCostlyStepsDownOneAtATime();
        testBackUp();
        testCapAverage();
        testLateFramesAreCounted();
        testBehindOnlyWithFewProcessors();
        testLadder();
        testBudgets();
    }
}
