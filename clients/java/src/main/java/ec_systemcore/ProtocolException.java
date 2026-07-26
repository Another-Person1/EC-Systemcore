package ec_systemcore;

/** Indicates malformed, unsupported, or unsafe ECSC version 1 wire data. */
public final class ProtocolException extends Exception {
  private static final long serialVersionUID = 1L;

  public ProtocolException(String message) {
    super(message);
  }

  public ProtocolException(String message, Throwable cause) {
    super(message, cause);
  }
}
