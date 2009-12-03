/* Copyright (c) 2008-2009, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

package java.lang;

import avian.AnnotationInvocationHandler;

import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.lang.reflect.Field;
import java.lang.reflect.Modifier;
import java.lang.reflect.Type;
import java.lang.reflect.TypeVariable;
import java.lang.reflect.Proxy;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.GenericDeclaration;
import java.lang.reflect.AnnotatedElement;
import java.lang.annotation.Annotation;
import java.io.InputStream;
import java.io.IOException;
import java.net.URL;
import java.util.Map;
import java.util.HashMap;
import java.security.ProtectionDomain;
import java.security.Permissions;
import java.security.AllPermission;

public final class Class <T>
  implements Type, GenericDeclaration, AnnotatedElement
{
  private static final int PrimitiveFlag = 1 << 5;

  private short flags;
  public short vmFlags;
  private short fixedSize;
  private byte arrayElementSize;
  private byte arrayDimensions;
  private int[] objectMask;
  private byte[] name;
  private byte[] sourceFile;
  public Class super_;
  public Object[] interfaceTable;
  public Method[] virtualTable;
  public Field[] fieldTable;
  public Method[] methodTable;
  public avian.ClassAddendum addendum;
  private Object staticTable;
  private ClassLoader loader;

  private Class() { }

  public String toString() {
    return getName();
  }

  private static byte[] replace(int a, int b, byte[] s, int offset,
                                int length)
  {
    byte[] array = new byte[length];
    for (int i = 0; i < length; ++i) {
      byte c = s[i];
      array[i] = (byte) (c == a ? b : c);
    }
    return array;
  }

  public String getName() {
    if (name == null) {
      if ((vmFlags & PrimitiveFlag) != 0) {
        if (this == primitiveClass('V')) {
          name = "void\0".getBytes();
        } else if (this == primitiveClass('Z')) {
          name = "boolean\0".getBytes();
        } else if (this == primitiveClass('B')) {
          name = "byte\0".getBytes();
        } else if (this == primitiveClass('C')) {
          name = "char\0".getBytes();
        } else if (this == primitiveClass('S')) {
          name = "short\0".getBytes();
        } else if (this == primitiveClass('I')) {
          name = "int\0".getBytes();
        } else if (this == primitiveClass('F')) {
          name = "float\0".getBytes();
        } else if (this == primitiveClass('J')) {
          name = "long\0".getBytes();
        } else if (this == primitiveClass('D')) {
          name = "double\0".getBytes();
        } else {
          throw new AssertionError();
        }
      } else {
        throw new AssertionError();
      }
    }

    return new String
      (replace('/', '.', name, 0, name.length - 1), 0, name.length - 1, false);
  }

  public String getCanonicalName() {
    if ((vmFlags & PrimitiveFlag) != 0) {
      return getName();
    } else if (isArray()) {
      return getComponentType().getCanonicalName() + "[]";
    } else {
      return getName().replace('$', '.');
    }
  }

  public String getSimpleName() {
    if ((vmFlags & PrimitiveFlag) != 0) {
      return getName();
    } else if (isArray()) {
      return getComponentType().getSimpleName() + "[]";
    } else {
      String name = getCanonicalName();
      int index = name.lastIndexOf('.');
      if (index >= 0) {
        return name.substring(index + 1);
      } else {
        return name;
      }
    }
  }

  public Object staticTable() {
    return staticTable;
  }

  public T newInstance()
    throws IllegalAccessException, InstantiationException
  {
    try {
      return (T) getConstructor().newInstance();
    } catch (NoSuchMethodException e) {
      throw new RuntimeException(e);
    } catch (InvocationTargetException e) {
      throw new RuntimeException(e);
    }
  }

  public static Class forName(String name) throws ClassNotFoundException {
    return forName
      (name, true, Method.getCaller().getDeclaringClass().getClassLoader());
  }

  public static Class forName(String name, boolean initialize,
                              ClassLoader loader)
    throws ClassNotFoundException
  {
    if (loader == null) {
      loader = Class.class.loader;
    }
    Class c = loader.loadClass(name);
    avian.SystemClassLoader.link(c, loader);
    if (initialize) {
      c.initialize();
    }
    return c;
  }

  private static native Class primitiveClass(char name);

  private native void initialize();
  
  public static Class forCanonicalName(String name) {
    return forCanonicalName(null, name);
  }

  public static Class forCanonicalName(ClassLoader loader, String name) {
    try {
      if (name.startsWith("[")) {
        return forName(name, true, loader);
      } else if (name.startsWith("L")) {
        return forName(name.substring(1, name.length() - 1), true, loader);
      } else {
        if (name.length() == 1) {
          return primitiveClass(name.charAt(0));
        } else {
          throw new ClassNotFoundException(name);
        }
      }
    } catch (ClassNotFoundException e) {
      throw new RuntimeException(e);
    }
  }

  public Class getComponentType() {
    if (isArray()) {
      String n = getName();
      if ("[Z".equals(n)) {
        return primitiveClass('Z');
      } else if ("[B".equals(n)) {
        return primitiveClass('B');
      } else if ("[S".equals(n)) {
        return primitiveClass('S');
      } else if ("[C".equals(n)) {
        return primitiveClass('C');
      } else if ("[I".equals(n)) {
        return primitiveClass('I');
      } else if ("[F".equals(n)) {
        return primitiveClass('F');
      } else if ("[J".equals(n)) {
        return primitiveClass('J');
      } else if ("[D".equals(n)) {
        return primitiveClass('D');
      }

      if (staticTable == null) throw new AssertionError(name);
      return (Class) staticTable;
    } else {
      return null;
    }
  }

  public native boolean isAssignableFrom(Class c);

  private Field findField(String name) {
    if (fieldTable != null) {
      avian.SystemClassLoader.link(this);

      for (int i = 0; i < fieldTable.length; ++i) {
        if (fieldTable[i].getName().equals(name)) {
          return fieldTable[i];
        }
      }
    }
    return null;
  }

  public Field getDeclaredField(String name) throws NoSuchFieldException {
    Field f = findField(name);
    if (f == null) {
      throw new NoSuchFieldException(name);
    } else {
      return f;
    }
  }

  public Field getField(String name) throws NoSuchFieldException {
    for (Class c = this; c != null; c = c.super_) {
      Field f = c.findField(name);
      if (f != null) {
        return f;
      }
    }
    throw new NoSuchFieldException(name);
  }

  private static boolean match(Class[] a, Class[] b) {
    if (a.length == b.length) {
      for (int i = 0; i < a.length; ++i) {
        if (! a[i].isAssignableFrom(b[i])) {
          return false;
        }
      }
      return true;
    } else {
      return false;
    }
  }

  private Method findMethod(String name, Class[] parameterTypes) {
    if (methodTable != null) {
      avian.SystemClassLoader.link(this);

      if (parameterTypes == null) {
        parameterTypes = new Class[0];
      }

      for (int i = 0; i < methodTable.length; ++i) {
        if (methodTable[i].getName().equals(name)
            && match(parameterTypes, methodTable[i].getParameterTypes()))
        {
          return methodTable[i];
        }
      }
    }
    return null;
  }

  public Method getDeclaredMethod(String name, Class ... parameterTypes)
    throws NoSuchMethodException
  {
    if (name.startsWith("<")) {
      throw new NoSuchMethodException(name);
    }
    Method m = findMethod(name, parameterTypes);
    if (m == null) {
      throw new NoSuchMethodException(name);
    } else {
      return m;
    }
  }

  public Method getMethod(String name, Class ... parameterTypes)
    throws NoSuchMethodException
  {
    if (name.startsWith("<")) {
      throw new NoSuchMethodException(name);
    }
    for (Class c = this; c != null; c = c.super_) {
      Method m = c.findMethod(name, parameterTypes);
      if (m != null) {
        return m;
      }
    }
    throw new NoSuchMethodException(name);
  }

  public Constructor getConstructor(Class ... parameterTypes)
    throws NoSuchMethodException
  {
    Method m = findMethod("<init>", parameterTypes);
    if (m == null) {
      throw new NoSuchMethodException();
    } else {
      return new Constructor(m);
    }
  }

  public Constructor getDeclaredConstructor(Class ... parameterTypes)
    throws NoSuchMethodException
  {
    Constructor c = null;
    Constructor[] constructors = getDeclaredConstructors();

    for (int i = 0; i < constructors.length; ++i) {
      if (match(parameterTypes, constructors[i].getParameterTypes())) {
        c = constructors[i];
      }
    }

    if (c == null) {
      throw new NoSuchMethodException();
    } else {
      return c;
    }
  }

  private int countConstructors(boolean publicOnly) {
    int count = 0;
    if (methodTable != null) {
      for (int i = 0; i < methodTable.length; ++i) {
        if (((! publicOnly)
             || ((methodTable[i].getModifiers() & Modifier.PUBLIC)) != 0)
            && methodTable[i].getName().equals("<init>"))
        {
          ++ count;
        }
      }
    }
    return count;
  }

  public Constructor[] getDeclaredConstructors() {
    Constructor[] array = new Constructor[countConstructors(false)];
    if (methodTable != null) {
      avian.SystemClassLoader.link(this);

      int index = 0;
      for (int i = 0; i < methodTable.length; ++i) {
        if (methodTable[i].getName().equals("<init>")) {
          array[index++] = new Constructor(methodTable[i]);
        }
      }
    }

    return array;
  }

  public Constructor[] getConstructors() {
    Constructor[] array = new Constructor[countConstructors(true)];
    if (methodTable != null) {
      avian.SystemClassLoader.link(this);

      int index = 0;
      for (int i = 0; i < methodTable.length; ++i) {
        if (((methodTable[i].getModifiers() & Modifier.PUBLIC) != 0)
            && methodTable[i].getName().equals("<init>"))
        {
          array[index++] = new Constructor(methodTable[i]);
        }
      }
    }

    return array;
  }

  public Field[] getDeclaredFields() {
    if (fieldTable != null) {
      Field[] array = new Field[fieldTable.length];
      System.arraycopy(fieldTable, 0, array, 0, fieldTable.length);
      return array;
    } else {
      return new Field[0];
    }
  }

  private int countPublicFields() {
    int count = 0;
    if (fieldTable != null) {
      for (int i = 0; i < fieldTable.length; ++i) {
        if (((fieldTable[i].getModifiers() & Modifier.PUBLIC)) != 0) {
          ++ count;
        }
      }
    }
    return count;
  }

  public Field[] getFields() {
    Field[] array = new Field[countPublicFields()];
    if (fieldTable != null) {
      avian.SystemClassLoader.link(this);

      int ai = 0;
      for (int i = 0; i < fieldTable.length; ++i) {
        if (((fieldTable[i].getModifiers() & Modifier.PUBLIC)) != 0) {
          array[ai++] = fieldTable[i];
        }
      }
    }
    return array;
  }

  private int countMethods(boolean publicOnly) {
    int count = 0;
    if (methodTable != null) {
      for (int i = 0; i < methodTable.length; ++i) {
        if (((! publicOnly)
             || ((methodTable[i].getModifiers() & Modifier.PUBLIC)) != 0)
            && (! methodTable[i].getName().startsWith("<")))
        {
          ++ count;
        }
      }
    }
    return count;
  }

  public Method[] getDeclaredMethods() {
    Method[] array = new Method[countMethods(false)];
    if (methodTable != null) {
      avian.SystemClassLoader.link(this);

      int ai = 0;
      for (int i = 0; i < methodTable.length; ++i) {
        if (! methodTable[i].getName().startsWith("<")) {
          array[ai++] = methodTable[i];
        }
      }
    }

    return array;
  }

  public Method[] getMethods() {
    Method[] array = new Method[countMethods(true)];
    if (methodTable != null) {
      avian.SystemClassLoader.link(this);

      int index = 0;
      for (int i = 0; i < methodTable.length; ++i) {
        if (((methodTable[i].getModifiers() & Modifier.PUBLIC) != 0)
            && (! methodTable[i].getName().startsWith("<")))
        {
          array[index++] = methodTable[i];
        }
      }
    }

    return array;
  }

  public Class[] getInterfaces() {
    if (interfaceTable != null) {
      avian.SystemClassLoader.link(this);

      int stride = (isInterface() ? 1 : 2);
      Class[] array = new Class[interfaceTable.length / stride];
      for (int i = 0; i < array.length; ++i) {
        array[i] = (Class) interfaceTable[i * stride];
      }
      return array;
    } else {
      return new Class[0];
    }
  }

  public T[] getEnumConstants() {
    if (Enum.class.isAssignableFrom(this)) {
      try {
        return (T[]) getMethod("values").invoke(null);
      } catch (Exception e) {
        throw new Error();
      }
    } else {
      return null;
    }
  }

  public ClassLoader getClassLoader() {
    return loader;
  }

  public int getModifiers() {
    return flags;
  }

  public boolean isInterface() {
    return (flags & Modifier.INTERFACE) != 0;
  }

  public Class getSuperclass() {
    return super_;
  }

  public boolean isArray() {
    return arrayDimensions != 0;
  }

  public boolean isInstance(Object o) {
    return o != null && isAssignableFrom(o.getClass());
  }

  public boolean isPrimitive() {
    return (vmFlags & PrimitiveFlag) != 0;
  }

  public URL getResource(String path) {
    if (! path.startsWith("/")) {
      String name = new String(this.name, 0, this.name.length - 1, false);
      int index = name.lastIndexOf('/');
      if (index >= 0) {
        path = name.substring(0, index) + "/" + path;
      }
    }
    return getClassLoader().getResource(path);
  }

  public InputStream getResourceAsStream(String path) {
    URL url = getResource(path);
    try {
      return (url == null ? null : url.openStream());
    } catch (IOException e) {
      return null;
    }
  }

  public boolean desiredAssertionStatus() {
    return false;
  }

  public <T> Class<? extends T> asSubclass(Class<T> c) {
    if (! c.isAssignableFrom(this)) {
      throw new ClassCastException();
    }

    return (Class<? extends T>) this;
  }

  public T cast(Object o) {
    return (T) o;
  }

  public Object[] getSigners() {
    return addendum == null ? null : addendum.signers;
  }

  public Package getPackage() {
    if ((vmFlags & PrimitiveFlag) != 0 || isArray()) {
      return null;
    } else {
      String name = getCanonicalName();
      int index = name.lastIndexOf('.');
      if (index >= 0) {
        return new Package(name.substring(0, index),
                           null, null, null, null, null, null, null, null);
      } else {
        return null;
      }
    }
  }

  public boolean isAnnotationPresent
    (Class<? extends Annotation> class_)
  {
    return getAnnotation(class_) != null;
  }

  private Annotation getAnnotation(Object[] a) {
    if (a[0] == null) {
      a[0] = Proxy.newProxyInstance
        (loader, new Class[] { (Class) a[1] },
         new AnnotationInvocationHandler(a));
    }
    return (Annotation) a[0];
  }

  public <T extends Annotation> T getAnnotation(Class<T> class_) {
    for (Class c = this; c != null; c = c.super_) {
      if (c.addendum != null && c.addendum.annotationTable != null) {
        avian.SystemClassLoader.link(c, c.loader);
        
        Object[] table = (Object[]) c.addendum.annotationTable;
        for (int i = 0; i < table.length; ++i) {
          Object[] a = (Object[]) table[i];
          if (a[1] == class_) {
            return (T) c.getAnnotation(a);
          }
        }
      }
    }
    return null;
  }

  public Annotation[] getDeclaredAnnotations() {
    if (addendum != null && addendum.annotationTable != null) {
      avian.SystemClassLoader.link(this);

      Object[] table = (Object[]) addendum.annotationTable;
      Annotation[] array = new Annotation[table.length];
      for (int i = 0; i < table.length; ++i) {
        array[i] = getAnnotation((Object[]) table[i]);
      }
      return array;
    } else {
      return new Annotation[0];
    }
  }

  private int countAnnotations() {
    int count = 0;
    for (Class c = this; c != null; c = c.super_) {
      if (c.addendum != null && c.addendum.annotationTable != null) {
        count += ((Object[]) c.addendum.annotationTable).length;
      }
    }
    return count;
  }

  public Annotation[] getAnnotations() {
    Annotation[] array = new Annotation[countMethods(true)];
    int i = 0;
    for (Class c = this; c != null; c = c.super_) {
      if (c.addendum != null && c.addendum.annotationTable != null) {
        Object[] table = (Object[]) c.addendum.annotationTable;
        for (int j = 0; j < table.length; ++j) {
          array[i++] = getAnnotation((Object[]) table[j]);
        }
      }
    }

    return array;
  }

  public boolean isEnum() {
    throw new UnsupportedOperationException();
  }

  public TypeVariable<Class<T>>[] getTypeParameters() {
    throw new UnsupportedOperationException();
  }

  public Method getEnclosingMethod() {
    throw new UnsupportedOperationException();
  }

  public Constructor getEnclosingConstructor() {
    throw new UnsupportedOperationException();
  }

  public Class getEnclosingClass() {
    throw new UnsupportedOperationException();
  }

  public Class[] getDeclaredClasses() {
    throw new UnsupportedOperationException();
  }

  public ProtectionDomain getProtectionDomain() {
    Permissions p = new Permissions();
    p.add(new AllPermission());
    return new ProtectionDomain(null, p);
  }

  // for GNU Classpath compatibility:
  void setSigners(Object[] signers) {
    if (signers != null && signers.length > 0) {
      if (addendum == null) {
        addendum = new avian.ClassAddendum();
      }
      addendum.signers = signers;
    }
  }
}
