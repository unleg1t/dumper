package demo;

import java.util.ArrayList;
import java.util.List;

/** A tiny long running program used by tests/run.sh. */
public class DemoApp {

    interface Greeter {
        String greet();
    }

    static class Inner {
        int value = 42;
    }

    static class Another {
        String text = "hello";
    }

    private static int counter;

    public static void main(String[] args) throws Exception {
        Greeter greeter = () -> "hello from a lambda";
        List<Object> keep = new ArrayList<>();
        keep.add(new Inner());
        keep.add(new Another());

        System.out.println("DEMO_PID=" + ProcessHandle.current().pid());
        System.out.println(greeter.greet() + " " + keep.size());

        while (true) {
            counter++;
            Thread.sleep(250);
        }
    }
}
