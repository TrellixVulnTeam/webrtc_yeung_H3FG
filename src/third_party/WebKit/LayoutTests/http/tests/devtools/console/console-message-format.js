// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

(async function() {
  TestRunner.addResult(`Tests that console logging uses proper message formatting.\n`);

  await TestRunner.loadModule('console_test_runner');
  await TestRunner.showPanel('console');
  await TestRunner.loadHTML(`
    <p id="p"></p>
  `);
  await TestRunner.evaluateInPagePromise(`
    console.log('Message format number %i, %d and %f', 1, 2, 3.5);
    console.log('Message %s for %s', 'format', 'string');
    console.log('Object %o', {'foo' : 'bar' });
    console.log("Floating as integers: %d %i", 42.5, 42.5);
    console.log("Floating as is: %f", 42.5);
    console.log("Non-numbers as numbers: %d %i %f", document, null, "document");
    console.log("String as is: %s", "string");
    console.log("Object as string: %s", document);
  `);

  ConsoleTestRunner.dumpConsoleMessages();
  TestRunner.completeTest();
})();
