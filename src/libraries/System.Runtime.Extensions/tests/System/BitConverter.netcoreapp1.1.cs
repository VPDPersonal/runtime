// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

using Xunit;

namespace System.Tests
{
    public static partial class BitConverterTests
    {
        [Fact]
        public static void SingleToInt32Bits()
        {
            Single input = 12345.63f;
            Int32 result = BitConverter.SingleToInt32Bits(input);
            Assert.Equal(1178658437, result);
            Single roundtripped = BitConverter.Int32BitsToSingle(result);
            Assert.Equal(input, roundtripped);
        }
    }
}
