// Copyright (c) 2018, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'package:analysis_server/src/services/correction/assist.dart';
import 'package:analyzer_plugin/utilities/assist/assist.dart';
import 'package:test_reflective_loader/test_reflective_loader.dart';

import 'assist_processor.dart';

main() {
  defineReflectiveSuite(() {
    defineReflectiveTests(ConvertIntoAsyncBodyTest);
  });
}

@reflectiveTest
class ConvertIntoAsyncBodyTest extends AssistProcessorTest {
  @override
  AssistKind get kind => DartAssistKind.CONVERT_INTO_ASYNC_BODY;

  test_async() async {
    await resolveTestUnit('''
import 'dart:async';
Future<String> f() async => '';
''');
    await assertNoAssistAt('=>');
  }

  test_asyncStar() async {
    await resolveTestUnit('''
import 'dart:async';
Stream<String> f() async* {}
''');
    await assertNoAssistAt('{}');
  }

  test_closure() async {
    await resolveTestUnit('''
main() {
  f(() => 123);
}
f(g) {}
''');
    await assertHasAssistAt('=>', '''
main() {
  f(() async => 123);
}
f(g) {}
''');
  }

  test_constructor() async {
    await resolveTestUnit('''
class C {
  C() {}
}
''');
    await assertNoAssistAt('{}');
  }

  test_function() async {
    await resolveTestUnit('''
String f() => '';
''');
    await assertHasAssistAt('=>', '''
Future<String> f() async => '';
''');
  }

  test_getter_expression_noSpace() async {
    await resolveTestUnit('''
class C {
  int get g=>0;
}
''');
    await assertHasAssistAt('get g', '''
class C {
  Future<int> get g async =>0;
}
''');
  }

  test_inBody_block() async {
    await resolveTestUnit('''
class C {
  void foo() {
    print(42);
  }
}
''');
    await assertNoAssistAt('print');
  }

  test_inBody_expression() async {
    await resolveTestUnit('''
class C {
  void foo() => print(42);
}
''');
    await assertNoAssistAt('print');
  }

  test_method() async {
    await resolveTestUnit('''
class C {
  int m() { return 0; }
}
''');
    await assertHasAssistAt('{ return', '''
class C {
  Future<int> m() async { return 0; }
}
''');
  }

  test_method_abstract() async {
    await resolveTestUnit('''
abstract class C {
  int m();
}
''');
    await assertHasAssistAt('m()', '''
abstract class C {
  Future<int> m();
}
''');
  }

  test_method_noReturnType() async {
    await resolveTestUnit('''
class C {
  m() { return 0; }
}
''');
    await assertHasAssistAt('{ return', '''
class C {
  m() async { return 0; }
}
''');
  }

  test_syncStar() async {
    await resolveTestUnit('''
Iterable<String> f() sync* {}
''');
    await assertNoAssistAt('{}');
  }
}
