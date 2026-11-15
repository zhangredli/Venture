import android.content.Context;
import android.net.nsd.*;
import android.os.*;
import java.net.InetAddress;
import java.util.concurrent.atomic.AtomicInteger;

/** SDK reflection must work even when the host has no Android NSD daemon. */
public final class NsdTest {
  static void check(boolean value) { if (!value) throw new AssertionError(); }
  // Unity scans every method to find GetCountryCodeFromSIM; an unrelated NSD
  // method signature must not prevent the entire plugin class from loading.
  public static class DeviceInfo {
    public static String getCountry(Context context) {
      return ((android.telephony.TelephonyManager) context.getSystemService(Context.TELEPHONY_SERVICE)).getSimCountryIso();
    }
    public static void register(NsdManager.RegistrationListener listener, NsdServiceInfo info) {}
  }
  public static void main(String[] args) throws Exception {
    check(DeviceInfo.class.getDeclaredMethods().length == 2);
    Context context = new Context();
    check(DeviceInfo.getCountry(context).equals(""));
    NsdManager nsd = context.getSystemService(NsdManager.class);
    check(nsd != null && nsd == context.getSystemService(Context.NSD_SERVICE));
    NsdServiceInfo info = new NsdServiceInfo();
    info.setServiceName("venture-fixture"); info.setServiceType("_fixture._tcp.");
    info.setHost(InetAddress.getLoopbackAddress()); info.setPort(12345);
    info.setAttribute("key", "value");
    Parcel parcel = Parcel.obtain();
    info.writeToParcel(parcel, 0); parcel.setDataPosition(0);
    NsdServiceInfo copy = NsdServiceInfo.CREATOR.createFromParcel(parcel);
    check(copy.getServiceName().equals(info.getServiceName()));
    check(copy.getServiceType().equals(info.getServiceType()));
    check(copy.getHost().equals(info.getHost()) && copy.getPort() == info.getPort());
    check(new String(copy.getAttributes().get("key"), java.nio.charset.StandardCharsets.UTF_8).equals("value"));
    parcel.recycle();
    Looper.prepareMainLooper();
    AtomicInteger failures = new AtomicInteger();
    nsd.registerService(info, NsdManager.PROTOCOL_DNS_SD, new NsdManager.RegistrationListener() {
      public void onRegistrationFailed(NsdServiceInfo service, int error) {
        check(Looper.getMainLooper().isCurrentThread());
        check(service == info && error == NsdManager.FAILURE_INTERNAL_ERROR); failures.incrementAndGet();
      }
      public void onUnregistrationFailed(NsdServiceInfo service, int error) { throw new AssertionError(); }
      public void onServiceRegistered(NsdServiceInfo service) { throw new AssertionError("No daemon: cannot advertise success"); }
      public void onServiceUnregistered(NsdServiceInfo service) { throw new AssertionError(); }
    });
    nsd.resolveService(info, new NsdManager.ResolveListener() {
      public void onResolveFailed(NsdServiceInfo service, int error) {
        check(service == info && error == NsdManager.FAILURE_INTERNAL_ERROR); failures.incrementAndGet();
      }
      public void onServiceResolved(NsdServiceInfo service) { throw new AssertionError(); }
    });
    check(failures.get() == 0); // Failure is asynchronous, like the framework.
    new Handler(Looper.getMainLooper()).post(() -> {
      check(failures.get() == 2); Looper.myLooper().quit();
    });
    Looper.loop();
    System.out.println("NSD reflection, service lookup, parcel and unavailable callbacks passed");
  }
}
