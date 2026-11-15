package android.net.nsd;

import android.os.Parcel;
import android.os.Parcelable;
import java.net.InetAddress;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.Map;

/** Android DNS-SD service description. No service is advertised by Venture. */
public final class NsdServiceInfo implements Parcelable {
  private String name, type;
  private InetAddress host;
  private int port;
  private final Map<String, byte[]> attributes = new LinkedHashMap<>();
  public String getServiceName() { return name; }
  public void setServiceName(String value) { name = value; }
  public String getServiceType() { return type; }
  public void setServiceType(String value) { type = value; }
  public InetAddress getHost() { return host; }
  public void setHost(InetAddress value) { host = value; }
  public int getPort() { return port; }
  public void setPort(int value) {
    if (value < 0 || value > 65535) throw new IllegalArgumentException("Invalid port");
    port = value;
  }
  public void setAttribute(String key, String value) {
    setAttribute(key, value == null ? null : value.getBytes(java.nio.charset.StandardCharsets.UTF_8));
  }
  public void setAttribute(String key, byte[] value) {
    if (key == null || key.isEmpty()) throw new IllegalArgumentException("Empty key");
    for (int i = 0; i < key.length(); ++i)
      if (key.charAt(i) < 0x20 || key.charAt(i) > 0x7e || key.charAt(i) == '=')
        throw new IllegalArgumentException("Invalid TXT key");
    if (key.length() + (value == null ? 0 : value.length) >= 255)
      throw new IllegalArgumentException("TXT entry too large");
    attributes.put(key, value == null ? null : value.clone());
  }
  public void removeAttribute(String key) { attributes.remove(key); }
  public Map<String, byte[]> getAttributes() { return Collections.unmodifiableMap(attributes); }
  public int describeContents() { return 0; }
  public void writeToParcel(Parcel out, int flags) {
    out.writeString(name); out.writeString(type);
    out.writeByteArray(host == null ? null : host.getAddress());
    out.writeInt(port); out.writeInt(attributes.size());
    attributes.forEach((key, value) -> { out.writeString(key); out.writeByteArray(value); });
  }
  public static final Creator<NsdServiceInfo> CREATOR = new Creator<>() {
    public NsdServiceInfo createFromParcel(Parcel in) {
      NsdServiceInfo info = new NsdServiceInfo();
      info.name = in.readString(); info.type = in.readString();
      byte[] address = in.createByteArray();
      if (address != null) try { info.host = InetAddress.getByAddress(address); }
      catch (java.net.UnknownHostException e) { throw new IllegalArgumentException(e); }
      info.setPort(in.readInt());
      int count = in.readInt();
      if (count < 0 || count > 65535) throw new IllegalArgumentException("Invalid TXT count");
      for (int i = 0; i < count; ++i) info.setAttribute(in.readString(), in.createByteArray());
      return info;
    }
    public NsdServiceInfo[] newArray(int size) { return new NsdServiceInfo[size]; }
  };
}
