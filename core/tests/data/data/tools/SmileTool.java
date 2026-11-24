import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.fasterxml.jackson.dataformat.smile.SmileFactory;
import com.fasterxml.jackson.dataformat.smile.SmileGenerator;
import java.io.File;
import java.nio.file.Files;

/** encode <in.json> <out.sml> [shared] | decode <in.sml> <out.json> */
public class SmileTool {
  public static void main(String[] args) throws Exception {
    ObjectMapper json = new ObjectMapper();
    SmileFactory factory = new SmileFactory();
    if (args.length > 3 && args[3].equals("shared")) {
      factory.enable(SmileGenerator.Feature.CHECK_SHARED_STRING_VALUES);
      factory.enable(SmileGenerator.Feature.CHECK_SHARED_NAMES);
    } else if (args.length > 3 && args[3].equals("raw")) {
      factory.disable(SmileGenerator.Feature.CHECK_SHARED_NAMES);
      factory.enable(SmileGenerator.Feature.ENCODE_BINARY_AS_7BIT);
    }
    ObjectMapper smile = new ObjectMapper(factory);
    if (args[0].equals("encode")) {
      JsonNode tree = json.readTree(new File(args[1]));
      byte[] bytes = new byte[16];
      for (int i = 0; i < 16; ++i) bytes[i] = (byte) (i * 17);
      ((ObjectNode) tree).put("binary", bytes);
      Files.write(new File(args[2]).toPath(), smile.writeValueAsBytes(tree));
    } else {
      JsonNode tree = smile.readTree(new File(args[1]));
      Files.write(new File(args[2]).toPath(), json.writeValueAsBytes(tree));
    }
  }
}
