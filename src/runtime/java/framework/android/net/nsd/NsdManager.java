package android.net.nsd;

import android.os.Handler;
import android.os.Looper;
import java.util.Objects;

/** DNS-SD contracts for SDK reflection. Venture has no Android NSD daemon. */
public final class NsdManager {
  public static final int PROTOCOL_DNS_SD = 1;
  public static final int FAILURE_INTERNAL_ERROR = 0, FAILURE_ALREADY_ACTIVE = 3, FAILURE_MAX_LIMIT = 4;
  public static final int NSD_STATE_DISABLED = 1, NSD_STATE_ENABLED = 2;
  public static final String ACTION_NSD_STATE_CHANGED = "android.net.nsd.STATE_CHANGED";
  public static final String EXTRA_NSD_STATE = "nsd_state";
  public interface RegistrationListener {
    void onRegistrationFailed(NsdServiceInfo serviceInfo, int errorCode);
    void onUnregistrationFailed(NsdServiceInfo serviceInfo, int errorCode);
    void onServiceRegistered(NsdServiceInfo serviceInfo);
    void onServiceUnregistered(NsdServiceInfo serviceInfo);
  }
  public interface DiscoveryListener {
    void onStartDiscoveryFailed(String serviceType, int errorCode);
    void onStopDiscoveryFailed(String serviceType, int errorCode);
    void onDiscoveryStarted(String serviceType);
    void onDiscoveryStopped(String serviceType);
    void onServiceFound(NsdServiceInfo serviceInfo);
    void onServiceLost(NsdServiceInfo serviceInfo);
  }
  public interface ResolveListener {
    void onResolveFailed(NsdServiceInfo serviceInfo, int errorCode);
    void onServiceResolved(NsdServiceInfo serviceInfo);
  }
  private void unavailable(Runnable callback) {
    new Handler(Looper.getMainLooper()).post(callback);
  }
  private static void protocol(int protocol) {
    if (protocol != PROTOCOL_DNS_SD) throw new IllegalArgumentException("Unsupported protocol");
  }
  public void registerService(NsdServiceInfo info, int protocol, RegistrationListener listener) {
    Objects.requireNonNull(info); Objects.requireNonNull(listener); protocol(protocol);
    if (info.getServiceName() == null || info.getServiceName().isEmpty() ||
        info.getServiceType() == null || info.getServiceType().isEmpty() || info.getPort() == 0)
      throw new IllegalArgumentException("Incomplete service");
    unavailable(() -> listener.onRegistrationFailed(info, FAILURE_INTERNAL_ERROR));
  }
  public void unregisterService(RegistrationListener listener) {
    Objects.requireNonNull(listener);
    throw new IllegalArgumentException("Listener not registered");
  }
  public void discoverServices(String serviceType, int protocol, DiscoveryListener listener) {
    Objects.requireNonNull(listener); protocol(protocol);
    if (serviceType == null || serviceType.isEmpty()) throw new IllegalArgumentException("Empty type");
    unavailable(() -> listener.onStartDiscoveryFailed(serviceType, FAILURE_INTERNAL_ERROR));
  }
  public void stopServiceDiscovery(DiscoveryListener listener) {
    Objects.requireNonNull(listener);
    throw new IllegalArgumentException("Listener not registered");
  }
  public void resolveService(NsdServiceInfo info, ResolveListener listener) {
    Objects.requireNonNull(info); Objects.requireNonNull(listener);
    unavailable(() -> listener.onResolveFailed(info, FAILURE_INTERNAL_ERROR));
  }
}
