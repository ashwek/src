int main(int argc, char** argv) {
  int nx = 2;
  void theerror(double a[][nx+1]); // { dg-error "" }
  double** a;
  theerror(a); // { dg-error "" }
  return 0;
}
