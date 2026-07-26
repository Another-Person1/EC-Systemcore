package ec_systemcore;

import ec_systemcore.SystemCoreTypes.BusInfo;
import ec_systemcore.SystemCoreTypes.Health;
import ec_systemcore.SystemCoreTypes.OutputsDisabled;
import ec_systemcore.SystemCoreTypes.PdoInput;
import ec_systemcore.SystemCoreTypes.Status;

/**
 * Event callbacks dispatched away from the socket I/O thread.
 *
 * <p>Exceptions thrown by an implementation are isolated and cannot stop heartbeats or reconnects.
 */
public interface SystemCoreListener {
  default void onHealthChanged(Health health) {}

  default void onStatus(Status status) {}

  default void onBusInfo(BusInfo busInfo) {}

  default void onPdoInput(PdoInput input) {}

  default void onOutputsDisabled(OutputsDisabled disabled) {}
}
