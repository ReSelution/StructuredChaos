import SC.Logger;

using Logger = SC::ChaosLogger<"Test">;
int main(int argc, char *argv[]) {
  Logger::info("String {}", 1);
  Logger::shutdown();
  return 0;
}
