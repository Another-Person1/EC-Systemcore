import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;
import ec_systemcore.SystemCoreClient;
import ec_systemcore.SystemCoreConfig;
import ec_systemcore.SystemCoreTypes.ClientRole;
import ec_systemcore.SystemCoreTypes.HealthState;

/**
 * Framework-neutral example for the Java 25 robot runtime. The SDK bytecode is
 * compiled with {@code --release 17} and runs unchanged on Java 25. Connect
 * robotEnable to the FRC/FTC framework's authoritative enabled state.
 */
public final class RobotLoopExample implements AutoCloseable {
  private final AtomicBoolean robotEnable = new AtomicBoolean();
  private final SystemCoreClient ethercat =
      SystemCoreClient.connect(
          SystemCoreConfig.builder()
              .requestedRole(ClientRole.CONTROLLER)
              .subscribeInputs(true)
              .inputPeriodMillis(20)
              .build());
  private final AtomicBoolean enableRequested = new AtomicBoolean();
  private final AtomicLong observedGeneration = new AtomicLong();
  private byte[] inputBuffer = new byte[0];

  public void setRobotEnabled(boolean enabled) {
    robotEnable.set(enabled);
  }

  /** Call from the normal robot periodic loop; this method never waits for IPC. */
  public void periodic(byte[] completeOutputImage) {
    long generation = ethercat.generation();
    if (observedGeneration.getAndSet(generation) != generation) {
      enableRequested.set(false);
    }
    if (!robotEnable.get()) {
      enableRequested.set(false);
      if (ethercat.outputsEnabled()) {
        ethercat.disableOutputs();
      }
      return;
    }

    if (!enableRequested.get()
        && ethercat.health().state() == HealthState.CONTROLLING_DISABLED) {
      enableRequested.set(true);
      ethercat.enableOutputs().whenComplete(
          (ack, failure) -> {
            if (failure != null || !ack.ok()) {
              if (ethercat.generation() == generation) {
                enableRequested.set(false);
              }
              System.err.println(
                  failure != null
                      ? "EtherCAT enable failed: " + failure.getMessage()
                      : "EtherCAT enable rejected: " + ack.status());
            }
          });
    }

    if (ethercat.outputsEnabled()) {
      ethercat.tryWriteOutput(0, 1, 0, completeOutputImage);
    }
    ethercat.latestInput(0, 1).ifPresent(
        input -> {
          if (inputBuffer.length != input.size()) {
            inputBuffer = new byte[input.size()];
          }
          input.copyDataTo(inputBuffer);
          consumeInput(inputBuffer);
        });
  }

  private void consumeInput(byte[] completeInputImage) {
    // Decode application-specific PDO fields here.
  }

  @Override
  public void close() {
    ethercat.close();
  }
}
